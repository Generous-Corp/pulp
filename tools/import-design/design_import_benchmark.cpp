#include <pulp/canvas/canvas.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/design_ir_io.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/widgets.hpp>

#include "test/harness/rt_allocation_probe.hpp"
#include "tools/import-design/design_import_benchmark_fixture.hpp"
#include "tools/import-design/design_import_benchmark_measure.hpp"
#include "tools/import-design/design_import_benchmark_targets.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#include <unistd.h>
#endif

using namespace pulp::view;
using namespace pulp::design_import_bench;

namespace {

constexpr const char* kFixtureName = "phase5-imported-plugin-panel";

using Clock = std::chrono::steady_clock;

struct Config {
    std::string lane = "live";
    int idle_ms = 60000;
    int interactive_ms = 60000;
    int target_fps = 60;
    int warmup_frames = 0;
    std::filesystem::path output_path;
    // An importer-produced DesignIR to measure instead of the built-in panel.
    std::filesystem::path ir_path;
    // The materialized runtime bundle built from the same source as `ir_path`;
    // the live lane mounts it as the behavior realm under the DesignIR.
    std::filesystem::path bundle_path;
    // When set, each phase runs exactly this many frames without pacing, so
    // every counter in the receipt is reproducible run to run.
    std::optional<int> frames;
    // Settled build/run/teardown cycles after the measured phases; the memory
    // slope across them shows per-cycle growth a single run cannot.
    std::optional<int> cycles;
    // Planted negative: leak one view past teardown, to prove the terminal
    // readback can fail.
    bool plant_leak = false;
    // Planted negative: leak a fixed block per cycle, to prove the cycle slope
    // sees growth.
    bool plant_cycle_leak = false;
    // Planted negative: report this one-minute load instead of the measured
    // one, to prove a contended run is marked confounded.
    std::optional<double> plant_load;
    // recording | skia-raster | skia-graphite
    std::string canvas = "recording";
    // Exit non-zero when the steady-state resource gate fails.
    bool require_steady_state = false;
    // Planted negative: add a view that decodes an encoded image on every
    // paint, to prove the steady-state gate fails on per-frame decodes.
    bool plant_frame_decode = false;
    // Off measures the uncached text path: every paint shapes its text.
    bool paragraph_cache = true;
};

// Per-frame duration of one pipeline stage.
struct StageStats {
    double median_ms = 0.0;
    double p95_ms = 0.0;
    double max_ms = 0.0;
};

// Lap clock over one frame: update (the fixture's state change), layout,
// record (painting the tree into the frame's canvas) and submit (handing the
// frame to its target). Verification runs after the frame and is in none of
// them. The stages are laps of one clock, so they sum to the frame; the
// residual is the frame time the laps do not cover, which bounds the
// instrument's own overhead.
struct StageMetrics {
    StageStats update;
    StageStats layout;
    StageStats record;
    StageStats submit;
    double residual_ms_max = 0.0;
    // Sampled frames whose residual exceeded the threshold below.
    std::uint64_t frames_over_residual = 0;
};

// Resource work over a phase's sampled frames. `frames_after_first_with_work`
// counts sampled frames after the first that did any of it: the first frame
// of a phase may legitimately build what later frames reuse.
struct ResourceSeries {
    std::uint64_t total = 0;
    std::uint64_t per_frame_max = 0;
    std::uint64_t frames_after_first_with_work = 0;

    void add(std::uint64_t delta, bool first) {
        total += delta;
        per_frame_max = std::max(per_frame_max, delta);
        if (!first && delta != 0)
            ++frames_after_first_with_work;
    }
};

struct PhaseResources {
    ResourceSeries image_decodes;
    ResourceSeries gpu_uploads;
    ResourceSeries paragraph_builds;
    // Paragraph builds of text the frame's update did not write: re-shaping
    // that a cache should have served.
    ResourceSeries paragraph_builds_unwritten;
    ResourceSeries scene_records;
    ResourceSeries scene_replays;
    ResourceSeries image_cache_hits;
    ResourceSeries image_cache_builds;
};

// A frame's residual is over threshold above 50 us or 2% of the frame,
// whichever is larger. The stage split is reported inconsistent when more
// than 5% of sampled frames are over it: a single frame preempted between two
// clock reads is the scheduler, a systematic residual is untimed work.
constexpr double kStageResidualFloorMs = 0.05;
constexpr double kStageResidualFraction = 0.02;
constexpr double kStageResidualFrameShare = 0.05;

struct PhaseMetrics {
    int duration_ms = 0;
    int samples = 0;
    double cpu_ms = 0.0;
    double cpu_frame_ms_median = 0.0;
    double cpu_frame_ms_p99 = 0.0;
    int warmup_frames = 0;
    double frame_ms_median = 0.0;
    double frame_ms_p95 = 0.0;
    double frame_ms_p99 = 0.0;
    double frame_ms_max = 0.0;
    double update_ms_median = 0.0;
    double update_ms_p95 = 0.0;
    double update_ms_p99 = 0.0;
    double update_ms_max = 0.0;
    StageMetrics stages;
    bool stages_consistent = true;
    MemoryStats rss;
    MemoryStats phys_footprint;
    std::uint64_t allocations_total = 0;
    std::uint64_t allocated_bytes_total = 0;
    double allocations_per_frame_median = 0.0;
    double allocations_per_frame_p95 = 0.0;
    std::uint64_t paint_commands_last = 0;
    std::uint64_t bridge_calls = 0;
    std::uint64_t layout_passes = 0;
    std::uint64_t paint_calls = 0;
    std::uint64_t paint_commands_total = 0;
    std::uint64_t invalidation_requests = 0;
    std::uint64_t unmarked_paint_mutations = 0;
    std::uint64_t views_constructed = 0;
    std::uint64_t views_destroyed = 0;
    std::uint64_t slot_identity_changes = 0;
    std::uint64_t slot_text_mismatches = 0;
    int js_evaluations = 0;
    PhaseResources resources;
    // Host conditions at the first and last frame of the phase.
    std::optional<HostSample> host_start;
    std::optional<HostSample> host_end;
};

struct StartupMetrics {
    double build_ms = 0.0;
    double first_frame_ms = 0.0;
    double first_frame_render_ms = 0.0;
    std::uint64_t first_frame_paint_commands = 0;
    Reading rss_after_first_frame;
};

std::string json_escape(std::string_view input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (char ch : input) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    std::ostringstream ss;
                    ss << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<int>(static_cast<unsigned char>(ch));
                    out += ss.str();
                } else {
                    out += ch;
                }
                break;
        }
    }
    return out;
}

double elapsed_ms(Clock::time_point start, Clock::time_point end = Clock::now()) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

double cpu_clock_delta_ms(std::clock_t start, std::clock_t end) {
    if (start == static_cast<std::clock_t>(-1) ||
        end == static_cast<std::clock_t>(-1) ||
        end < start) {
        return 0.0;
    }
    return 1000.0 * static_cast<double>(end - start) /
           static_cast<double>(CLOCKS_PER_SEC);
}

std::string host_name() {
    char buf[256] = {0};
#if defined(_WIN32)
    return "windows";
#else
    if (gethostname(buf, sizeof(buf) - 1) == 0 && buf[0] != '\0') return buf;
    return "unknown";
#endif
}

std::string platform_name() {
#if defined(__APPLE__) && defined(__aarch64__)
    return "darwin-arm64";
#elif defined(__APPLE__)
    return "darwin";
#elif defined(__linux__) && defined(__x86_64__)
    return "linux-x64";
#elif defined(_WIN32)
    return "windows";
#else
    return "unknown";
#endif
}

void place_absolute(IRNode& node, float left, float top, float width, float height) {
    node.style.position = "absolute";
    node.style.left = left;
    node.style.top = top;
    node.style.width = width;
    node.style.height = height;
}

IRNode text_node(std::string id,
                 std::string text,
                 float left,
                 float top,
                 float width,
                 float height) {
    IRNode node;
    node.type = "text";
    node.stable_anchor_id = std::move(id);
    node.text_content = std::move(text);
    node.style.font_size = 15.0f;
    node.style.color = "#e8e8f0";
    place_absolute(node, left, top, width, height);
    return node;
}

IRNode audio_node(std::string id,
                  AudioWidgetType type,
                  std::string label,
                  float value,
                  float left,
                  float top,
                  float width,
                  float height) {
    IRNode node;
    node.type = "frame"; // importers carry the control role in audio_widget, not in type
    node.stable_anchor_id = std::move(id);
    node.audio_widget = type;
    node.audio_label = std::move(label);
    node.audio_min = 0.0f;
    node.audio_max = 1.0f;
    node.audio_default = value;
    place_absolute(node, left, top, width, height);
    return node;
}

IRNode panel_node(std::string id,
                  float left,
                  float top,
                  float width,
                  float height,
                  std::string overflow = "hidden") {
    IRNode node;
    node.type = "frame";
    node.stable_anchor_id = std::move(id);
    node.layout.direction = LayoutDirection::column;
    node.style.background_color = "#1f2030";
    node.style.overflow = std::move(overflow);
    place_absolute(node, left, top, width, height);
    return node;
}

/// The fixture's palette, in the semantic vocabulary a captured design states
/// — an accent, a signal ramp, line and surface levels — never Pulp's widget
/// keys. Materialization maps those onto `knob.arc`, `meter.green` and the
/// rest, so the baked-native lane paints the fixture's own colours rather than
/// the framework defaults a design-less IR would leave every primitive on.
/// That default fallback is precisely what the `design-token-unmapped`
/// diagnostic reports, and a benchmark measuring the import path should not be
/// an instance of the defect it can detect.
///
/// The values are deliberately unlike Pulp's built-in theme (a warm accent, not
/// the built-in blue) so a regression that drops the palette shows up as colour,
/// not just as a re-appearing diagnostic.
void apply_bench_tokens(IRTokens& tokens) {
    auto& c = tokens.colors;
    c["accent"] = "#e0863c";
    c["accent-soft"] = "#6c7fd8";
    c["danger"] = "#e4574f";
    c["warning"] = "#e0a83c";
    c["success"] = "#4fbf7e";
    c["info"] = "#4fa3e4";

    c["signal-low"] = "#4fbf7e";
    c["signal-mid"] = "#e0a83c";
    c["signal-high"] = "#e4574f";
    c["signal-wave"] = "#e0863c";

    c["line-strong"] = "#3a3c52";
    c["line-soft"] = "#2a2c3e";
    c["border"] = "#34364a";

    c["text-strong"] = "#e8e8f0";
    c["text-muted"] = "#9a9bb0";
    c["text-faint"] = "#64657a";
    c["on-ink"] = "#1a1206";

    // `surface-panel` is the same value the panel nodes state inline, so the
    // themed surfaces and the fixture's own geometry agree.
    c["surface-app"] = "#14151f";
    c["surface-sunken"] = "#191a27";
    c["surface-panel"] = "#1f2030";
    c["surface-raised"] = "#262838";
    c["surface-overlay"] = "#2b2d3f";
}

DesignIR build_baked_ir(int frame = -1) {
    DesignIR ir;
    ir.source = DesignSource::jsx;
    ir.source_adapter = "phase5-benchmark-fixture";
    ir.capture_method = "hand-authored-live-equivalent";
    apply_bench_tokens(ir.tokens);

    ir.root.type = "frame";
    ir.root.stable_anchor_id = "bench-root";
    ir.root.layout.direction = LayoutDirection::column;
    ir.root.style.width = static_cast<float>(kFixtureWidth);
    ir.root.style.height = static_cast<float>(kFixtureHeight);

    auto panel = panel_node("bench-panel", 0, 0,
                            static_cast<float>(kFixtureWidth),
                            static_cast<float>(kFixtureHeight));
    panel.children.push_back(text_node("title", "Design Import Benchmark", 24, 18, 260, 28));
    panel.children.push_back(text_node("subtitle", "Live bridge vs baked native", 24, 45, 260, 20));
    panel.children.push_back(audio_node("drive", AudioWidgetType::knob, "Drive", 0.64f, 28, 84, 96, 96));
    panel.children.push_back(audio_node("tone", AudioWidgetType::knob, "Tone", 0.38f, 152, 84, 96, 96));
    panel.children.push_back(audio_node("mix", AudioWidgetType::fader, "Mix", 0.42f, 290, 72, 54, 160));
    auto output = audio_node("output", AudioWidgetType::meter, "Output", 0.55f, 380, 72, 42, 160);
    output.attributes["peak"] = "0.38";
    panel.children.push_back(std::move(output));

    auto xy = audio_node("shape", AudioWidgetType::xy_pad, "Shape", 0.5f, 456, 78, 130, 130);
    xy.attributes["x"] = "0.35";
    xy.attributes["y"] = "0.65";
    panel.children.push_back(std::move(xy));

    auto viewport = panel_node("preset-viewport", 28, 260, 560, 126, "scroll");
    auto content = panel_node("preset-content", 0, 0, 540, 420, "visible");
    for (int i = 0; i < 14; ++i) {
        std::ostringstream label;
        label << "Preset " << std::setw(2) << std::setfill('0') << (i + 1)
              << " - imported control row";
        content.children.push_back(text_node("preset-" + std::to_string(i),
                                             label.str(),
                                             12,
                                             static_cast<float>(10 + i * 28),
                                             360,
                                             22));
    }
    viewport.children.push_back(std::move(content));
    panel.children.push_back(std::move(viewport));

    ir.root.children.push_back(std::move(panel));

    if (frame >= 0) {
        const float t = static_cast<float>(frame) / 60.0f;
        const auto drive = 0.5f + 0.45f * std::sin(t * 1.7f);
        const auto tone = 0.5f + 0.35f * std::cos(t * 1.1f);
        const auto mix = 0.5f + 0.45f * std::sin(t * 0.9f);
        const auto output = 0.55f + 0.35f * std::sin(t * 1.3f);
        const auto x = 0.5f + 0.45f * std::sin(t * 0.7f);
        const auto y = 0.5f + 0.45f * std::cos(t * 0.8f);

        const auto find = [&](auto&& self, IRNode& node, std::string_view id) -> IRNode* {
            if (node.stable_anchor_id && *node.stable_anchor_id == id)
                return &node;
            for (auto& child : node.children) {
                if (auto* found = self(self, child, id))
                    return found;
            }
            return nullptr;
        };
        if (auto* node = find(find, ir.root, "drive"))
            node->audio_default = drive;
        if (auto* node = find(find, ir.root, "tone"))
            node->audio_default = tone;
        if (auto* node = find(find, ir.root, "mix"))
            node->audio_default = mix;
        if (auto* node = find(find, ir.root, "output")) {
            node->audio_default = output;
            node->attributes["peak"] = std::to_string(std::max(0.0f, output - 0.18f));
        }
        if (auto* node = find(find, ir.root, "shape")) {
            node->attributes["x"] = std::to_string(x);
            node->attributes["y"] = std::to_string(y);
        }
        if (auto* node = find(find, ir.root, "preset-content"))
            node->style.top = -static_cast<float>((frame * 3) % 180);
    }
    return ir;
}

View* find_by_id(View& root, std::string_view id) {
    if (root.id() == id) return &root;
    for (std::size_t i = 0; i < root.child_count(); ++i) {
        if (auto* found = find_by_id(*root.child_at(i), id)) return found;
    }
    return nullptr;
}

void configure_absolute(View& view,
                        std::string id,
                        float left,
                        float top,
                        float width,
                        float height) {
    view.set_id(id);
    view.set_anchor_id(std::move(id));
    view.set_position(View::Position::absolute);
    view.set_left(left);
    view.set_top(top);
    auto& flex = view.flex();
    flex.preferred_width = width;
    flex.dim_width = {width, DimensionUnit::px};
    flex.preferred_height = height;
    flex.dim_height = {height, DimensionUnit::px};
}

std::unique_ptr<Label> make_bench_label(std::string id,
                                        std::string text,
                                        float left,
                                        float top,
                                        float width,
                                        float height) {
    auto label = std::make_unique<Label>(std::move(text));
    configure_absolute(*label, std::move(id), left, top, width, height);
    label->set_font_size(15.0f);
    label->set_text_color(Color::rgba8(232, 232, 240, 255));
    return label;
}

template <typename Widget>
Widget* add_bench_widget(View& parent,
                         std::unique_ptr<Widget> widget,
                         std::string id,
                         float left,
                         float top,
                         float width,
                         float height) {
    auto* ptr = widget.get();
    configure_absolute(*widget, std::move(id), left, top, width, height);
    parent.add_child(std::move(widget));
    return ptr;
}

class BakedNativeFixture final : public BenchmarkFixture {
public:
    BakedNativeFixture() {
        auto ir = build_baked_ir();
        NativeMaterializeOptions options;
        std::vector<ImportDiagnostic> diagnostics;
        options.diagnostics_out = &diagnostics;
        root_ = build_native_view_tree(ir, ir.asset_manifest, options);
        if (!diagnostics.empty()) {
            std::ostringstream msg;
            msg << "baked-native materialization emitted " << diagnostics.size()
                << " diagnostics";
            if (!diagnostics.front().code.empty()) msg << "; first=" << diagnostics.front().code;
            if (!diagnostics.front().message.empty()) msg << ": " << diagnostics.front().message;
            throw std::runtime_error(msg.str());
        }
        if (!root_) throw std::runtime_error("baked-native materialization returned null root");
        root_->set_bounds({0, 0, static_cast<float>(kFixtureWidth), static_cast<float>(kFixtureHeight)});
        root_->layout_children();
    }

    View& root() override { return *root_; }

    void step(int frame) override {
        const float t = static_cast<float>(frame) / 60.0f;
        set_widget_value("drive", 0.5f + 0.45f * std::sin(t * 1.7f));
        set_widget_value("tone", 0.5f + 0.35f * std::cos(t * 1.1f));
        set_widget_value("mix", 0.5f + 0.45f * std::sin(t * 0.9f));

        if (auto* meter = dynamic_cast<Meter*>(find_by_id(*root_, "output"))) {
            const float value = 0.55f + 0.35f * std::sin(t * 1.3f);
            meter->set_level(value, std::max(0.0f, value - 0.18f));
        }
        if (auto* xy = dynamic_cast<XYPad*>(find_by_id(*root_, "shape"))) {
            xy->set_x(0.5f + 0.45f * std::sin(t * 0.7f));
            xy->set_y(0.5f + 0.45f * std::cos(t * 0.8f));
        }
        if (auto* content = find_by_id(*root_, "preset-content")) {
            content->set_top(-static_cast<float>((frame * 3) % 180));
        }
    }

    int js_evaluation_count() const override { return 0; }

private:
    void set_widget_value(std::string_view id, float value) {
        if (auto* view = find_by_id(*root_, id)) {
            if (auto* knob = dynamic_cast<Knob*>(view)) knob->set_value(value);
            else if (auto* fader = dynamic_cast<Fader*>(view)) fader->set_value(value);
            else if (auto* range = dynamic_cast<RangeSlider*>(view)) range->set_value(value);
        }
    }

    std::unique_ptr<View> root_;
};

// Control lane for the structural retained experiment. This deliberately
// rebuilds the same imported DesignIR on every interactive step, so any
// retained-vs-rebuild claim has a real full-rematerialization comparator. It
// is diagnostic only; the production baked-native lane above remains the
// direct-setter baseline.
class BakedNativeRebuildFixture final : public BenchmarkFixture {
  public:
    BakedNativeRebuildFixture() {
        rebuild(-1);
    }

    View& root() override {
        return *root_;
    }

    void step(int frame) override {
        rebuild(frame);
    }

    int js_evaluation_count() const override {
        return 0;
    }

  private:
    void rebuild(int frame) {
        auto ir = build_baked_ir(frame);
        NativeMaterializeOptions options;
        std::vector<ImportDiagnostic> diagnostics;
        options.diagnostics_out = &diagnostics;
        auto next = build_native_view_tree(ir, ir.asset_manifest, options);
        if (!diagnostics.empty()) {
            std::ostringstream msg;
            msg << "baked-native-rebuild materialization emitted " << diagnostics.size()
                << " diagnostics";
            if (!diagnostics.front().code.empty())
                msg << "; first=" << diagnostics.front().code;
            if (!diagnostics.front().message.empty())
                msg << ": " << diagnostics.front().message;
            throw std::runtime_error(msg.str());
        }
        if (!next)
            throw std::runtime_error("baked-native-rebuild returned null root");
        next->set_bounds(
            {0, 0, static_cast<float>(kFixtureWidth), static_cast<float>(kFixtureHeight)});
        next->layout_children();
        root_ = std::move(next);
    }

    std::unique_ptr<View> root_;
};

class BakedCppFixture final : public BenchmarkFixture {
public:
    BakedCppFixture() {
        root_ = build_imported_ui();
        root_->set_bounds({0, 0, static_cast<float>(kFixtureWidth), static_cast<float>(kFixtureHeight)});
        root_->layout_children();
    }

    View& root() override { return *root_; }

    void step(int frame) override {
        const float t = static_cast<float>(frame) / 60.0f;
        drive_->set_value(0.5f + 0.45f * std::sin(t * 1.7f));
        tone_->set_value(0.5f + 0.35f * std::cos(t * 1.1f));
        mix_->set_value(0.5f + 0.45f * std::sin(t * 0.9f));

        const float value = 0.55f + 0.35f * std::sin(t * 1.3f);
        output_->set_level(value, std::max(0.0f, value - 0.18f));
        shape_->set_x(0.5f + 0.45f * std::sin(t * 0.7f));
        shape_->set_y(0.5f + 0.45f * std::cos(t * 0.8f));
        preset_content_->set_top(-static_cast<float>((frame * 3) % 180));
    }

    int js_evaluation_count() const override { return 0; }

private:
    std::unique_ptr<View> build_imported_ui() {
        auto root = std::make_unique<View>();
        root->set_id("bench-root");
        root->set_anchor_id("bench-root");
        auto& root_flex = root->flex();
        root_flex.direction = FlexDirection::column;
        root_flex.preferred_width = static_cast<float>(kFixtureWidth);
        root_flex.dim_width = {static_cast<float>(kFixtureWidth), DimensionUnit::px};
        root_flex.preferred_height = static_cast<float>(kFixtureHeight);
        root_flex.dim_height = {static_cast<float>(kFixtureHeight), DimensionUnit::px};

        auto panel = std::make_unique<View>();
        configure_absolute(*panel, "bench-panel", 0, 0,
                           static_cast<float>(kFixtureWidth),
                           static_cast<float>(kFixtureHeight));
        panel->flex().direction = FlexDirection::column;
        panel->set_background_color(Color::rgba8(31, 32, 48, 255));
        panel->set_inheritable_text_color(Color::rgba8(232, 232, 240, 255));
        panel->set_inheritable_font_size(15.0f);
        panel->set_overflow(View::Overflow::hidden);

        panel->add_child(make_bench_label("title", "Design Import Benchmark", 24, 18, 260, 28));
        panel->add_child(make_bench_label("subtitle", "Live bridge vs baked native", 24, 45, 260, 20));

        auto drive = std::make_unique<Knob>();
        drive->set_label("Drive");
        drive->set_value(0.64f);
        drive->set_default_value(0.64f);
        drive_ = add_bench_widget(*panel, std::move(drive), "drive", 28, 84, 96, 96);

        auto tone = std::make_unique<Knob>();
        tone->set_label("Tone");
        tone->set_value(0.38f);
        tone->set_default_value(0.38f);
        tone_ = add_bench_widget(*panel, std::move(tone), "tone", 152, 84, 96, 96);

        auto mix = std::make_unique<Fader>();
        mix->set_label("Mix");
        mix->set_value(0.42f);
        mix_ = add_bench_widget(*panel, std::move(mix), "mix", 290, 72, 54, 160);

        auto output = std::make_unique<Meter>();
        output->set_level(0.55f, 0.38f);
        output_ = add_bench_widget(*panel, std::move(output), "output", 380, 72, 42, 160);

        auto shape = std::make_unique<XYPad>();
        shape->set_x(0.35f);
        shape->set_y(0.65f);
        shape_ = add_bench_widget(*panel, std::move(shape), "shape", 456, 78, 130, 130);

        auto viewport = std::make_unique<View>();
        configure_absolute(*viewport, "preset-viewport", 28, 260, 560, 126);
        viewport->set_background_color(Color::rgba8(31, 32, 48, 255));
        viewport->set_overflow(View::Overflow::scroll);

        auto content = std::make_unique<View>();
        preset_content_ = content.get();
        configure_absolute(*content, "preset-content", 0, 0, 540, 420);
        content->set_background_color(Color::rgba8(31, 32, 48, 255));
        content->set_overflow(View::Overflow::visible);
        for (int i = 0; i < 14; ++i) {
            std::ostringstream label;
            label << "Preset " << std::setw(2) << std::setfill('0') << (i + 1)
                  << " - imported control row";
            content->add_child(make_bench_label("preset-" + std::to_string(i),
                                                label.str(),
                                                12,
                                                static_cast<float>(10 + i * 28),
                                                360,
                                                22));
        }

        viewport->add_child(std::move(content));
        panel->add_child(std::move(viewport));
        root->add_child(std::move(panel));
        return root;
    }

    std::unique_ptr<View> root_;
    Knob* drive_ = nullptr;
    Knob* tone_ = nullptr;
    Fader* mix_ = nullptr;
    Meter* output_ = nullptr;
    XYPad* shape_ = nullptr;
    View* preset_content_ = nullptr;
};

class LiveFixture final : public BenchmarkFixture {
public:
    LiveFixture() {
        root_ = std::make_unique<View>();
        root_->set_bounds({0, 0, static_cast<float>(kFixtureWidth), static_cast<float>(kFixtureHeight)});
        engine_ = std::make_unique<ScriptEngine>();
        store_ = std::make_unique<pulp::state::StateStore>();
        bridge_ = std::make_unique<WidgetBridge>(*engine_, *root_, *store_);
        bridge_->load_script(live_fixture_script(), "phase5-benchmark-fixture");
        root_->layout_children();
    }

    View& root() override { return *root_; }

    void step(int frame) override {
        engine_->evaluate("__pulpBenchStep(" + std::to_string(frame) + ");");
        ++js_evaluations_;
    }

    int js_evaluation_count() const override { return js_evaluations_; }
    std::uint64_t bridge_call_count() const override {
        return bridge_->bridge_call_count();
    }

private:
    static std::string live_fixture_script() {
        return R"JS(
function place(id, x, y, w, h) {
  setPosition(id, 'absolute');
  setLeft(id, x);
  setTop(id, y);
  setFlex(id, 'width', w);
  setFlex(id, 'height', h);
}

	createPanel('bench-panel', '');
	place('bench-panel', 0, 0, 760, 420);
	setOverflow('bench-panel', 'hidden');
	setBackground('bench-panel', '#1f2030');
	setTextColor('bench-panel', '#e8e8f0');
	setFontSize('bench-panel', 15);

	createLabel('title', 'Design Import Benchmark', 'bench-panel');
	place('title', 24, 18, 260, 28);
createLabel('subtitle', 'Live bridge vs baked native', 'bench-panel');
place('subtitle', 24, 45, 260, 20);

createKnob('drive', 'bench-panel');
place('drive', 28, 84, 96, 96);
setLabel('drive', 'Drive');
setValue('drive', 0.64);

createKnob('tone', 'bench-panel');
place('tone', 152, 84, 96, 96);
setLabel('tone', 'Tone');
setValue('tone', 0.38);

createFader('mix', 'vertical', 'bench-panel');
place('mix', 290, 72, 54, 160);
setLabel('mix', 'Mix');
setValue('mix', 0.42);

createMeter('output', 'vertical', 'bench-panel');
place('output', 380, 72, 42, 160);
setMeterLevel('output', 0.55, 0.38);

createXYPad('shape', 'bench-panel');
place('shape', 456, 78, 130, 130);
setXY('shape', 0.35, 0.65);

	createPanel('preset-viewport', 'bench-panel');
	place('preset-viewport', 28, 260, 560, 126);
	setOverflow('preset-viewport', 'scroll');
	setBackground('preset-viewport', '#1f2030');

	createPanel('preset-content', 'preset-viewport');
	place('preset-content', 0, 0, 540, 420);
	setOverflow('preset-content', 'visible');
	setBackground('preset-content', '#1f2030');

for (var i = 0; i < 14; ++i) {
  var id = 'preset-' + i;
  var n = String(i + 1);
  if (n.length < 2) n = '0' + n;
  createLabel(id, 'Preset ' + n + ' - imported control row', 'preset-content');
  place(id, 12, 10 + i * 28, 360, 22);
}

function __pulpBenchStep(frame) {
  var t = frame / 60;
  setValue('drive', 0.5 + 0.45 * Math.sin(t * 1.7));
  setValue('tone', 0.5 + 0.35 * Math.cos(t * 1.1));
  setValue('mix', 0.5 + 0.45 * Math.sin(t * 0.9));
  var out = 0.55 + 0.35 * Math.sin(t * 1.3);
  setMeterLevel('output', out, Math.max(0, out - 0.18));
  setXY('shape', 0.5 + 0.45 * Math.sin(t * 0.7), 0.5 + 0.45 * Math.cos(t * 0.8));
  setTop('preset-content', -((frame * 3) % 180));
}

layout();
)JS";
    }

    std::unique_ptr<View> root_;
    std::unique_ptr<ScriptEngine> engine_;
    std::unique_ptr<pulp::state::StateStore> store_;
    std::unique_ptr<WidgetBridge> bridge_;
    int js_evaluations_ = 0;
};

// Planted negative for the steady-state resource gate: draws a 1x1 PNG from
// its encoded bytes on every paint, so every frame decodes. It goes through
// the canvas's uncached encoded-bytes entry point, which no cache can absorb.
class PlantedDecodeView final : public View {
  public:
    PlantedDecodeView() {
        set_id("planted-frame-decode");
        set_position(View::Position::absolute);
        set_left(0.0f);
        set_top(0.0f);
        flex().dim_width = {4.0f, DimensionUnit::px};
        flex().dim_height = {4.0f, DimensionUnit::px};
    }

    void paint(pulp::canvas::Canvas& canvas) override {
        static constexpr std::uint8_t kPng[] = {
            0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48,
            0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00,
            0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41, 0x54, 0x78,
            0x9c, 0x63, 0x78, 0xd0, 0x66, 0xf3, 0x1f, 0x00, 0x06, 0x8e, 0x02, 0xa2, 0x86, 0xc6,
            0xaa, 0xc4, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
        canvas.draw_image_from_data(kPng, sizeof(kPng), 0.0f, 0.0f, 4.0f, 4.0f);
    }
};

std::unique_ptr<BenchmarkFixture> make_fixture(const Config& config) {
    if (!config.ir_path.empty()) {
        if (config.lane == "live")
            return std::make_unique<IrLiveFixture>(config.ir_path, config.bundle_path);
        return std::make_unique<IrBoundFixture>(config.ir_path,
                                                config.lane == "baked-native-rebuild");
    }
    const auto& lane = config.lane;
    if (lane == "live") return std::make_unique<LiveFixture>();
    if (lane == "baked-native") return std::make_unique<BakedNativeFixture>();
    if (lane == "baked-native-rebuild")
        return std::make_unique<BakedNativeRebuildFixture>();
    if (lane == "baked-cpp") return std::make_unique<BakedCppFixture>();
    return nullptr;
}

// One frame through `target`: layout, record and submit, without the lap
// clock. Startup and tests use it; run_phase times the same three calls.
std::uint64_t render_frame(View& root, FrameTarget& target) {
    root.layout_children();
    root.paint_all(target.begin());
    return target.end();
}

std::uint64_t render_frame(View& root) {
    RecordingTarget target;
    return render_frame(root, target);
}

StageStats stage_stats(const std::vector<double>& values) {
    StageStats stats;
    if (values.empty())
        return stats;
    stats.median_ms = percentile(values, 50.0);
    stats.p95_ms = percentile(values, 95.0);
    stats.max_ms = *std::max_element(values.begin(), values.end());
    return stats;
}

struct PhaseOptions {
    // Null paints into a per-phase RecordingTarget.
    FrameTarget* target = nullptr;
    std::optional<double> planted_load;
};

PhaseMetrics run_phase(BenchmarkFixture& fixture, int duration_ms, std::optional<int> frames,
                       int target_fps, bool interactive, int warmup_frames = 0,
                       const PhaseOptions& options = {}) {
    PhaseMetrics metrics;
    metrics.duration_ms = frames ? 0 : duration_ms;
    metrics.warmup_frames = warmup_frames;
    if (frames ? *frames <= 0 : duration_ms <= 0)
        return metrics;

    RecordingTarget default_target;
    FrameTarget& target = options.target ? *options.target : default_target;
    metrics.host_start = sample_host(options.planted_load);
    const auto interval = std::chrono::duration<double>(1.0 / std::max(1, target_fps));
    const auto interval_duration = std::chrono::duration_cast<Clock::duration>(interval);
    const auto phase_start = Clock::now();
    const auto cpu_start = std::clock();
    auto next_frame = phase_start;
    // Work counters are read from the start of the first sampled frame, so
    // warmup work stays out of them exactly as it stays out of the per-frame
    // statistics. A phase that never leaves warmup counts nothing. Lifetime
    // and verification counters cover every frame: a mismatch during warmup
    // is still a mismatch.
    struct CounterOrigin {
        std::uint64_t bridge_calls = 0;
        std::uint64_t layout_passes = 0;
        std::uint64_t invalidation_requests = 0;
        std::uint64_t unmarked_paint_mutations = 0;
    };
    std::optional<CounterOrigin> counters_from;
    const auto constructed_start = View::constructed_count();
    const auto destroyed_start = View::destroyed_count();
    const auto identity_start = fixture.slot_identity_change_count();
    double verify_cpu_ms = 0.0;
    const auto mismatch_start = fixture.slot_text_mismatch_count();
    std::vector<double> frame_ms;
    std::vector<double> frame_cpu_ms;
    std::vector<double> update_ms;
    std::vector<double> layout_ms;
    std::vector<double> record_ms;
    std::vector<double> submit_ms;
    std::vector<double> frame_allocations;
    // The text this frame's update wrote, and the builds of any other text.
    // Ordered with a transparent comparator so the observer looks text up
    // without allocating inside the measured frame.
    std::set<std::string, std::less<>> written;
    std::uint64_t unwritten_builds = 0;
    set_paragraph_build_observer([&](std::string_view text) {
        if (written.find(text) == written.end())
            ++unwritten_builds;
    });
    struct ObserverReset {
        ~ObserverReset() {
            set_paragraph_build_observer({});
        }
    } observer_reset;
    MemorySeries rss;
    MemorySeries footprint;

    int frame = 0;
    do {
        // Warmup frames run the same work but are excluded from every
        // per-frame statistic so first-touch cache and allocator effects do
        // not land in the median or tail.
        const bool sampled = frame >= warmup_frames;
        if (sampled && !counters_from) {
            counters_from =
                CounterOrigin{fixture.bridge_call_count(), View::layout_pass_count(),
                              View::damage_request_count(), View::unmarked_paint_mutation_count()};
        }
        std::size_t allocations = 0;
        std::size_t allocated_bytes = 0;
        Clock::time_point laps[4];
        written.clear();
        if (interactive)
            for (auto& text : fixture.text_written_by_step(frame))
                written.insert(std::move(text));
        const auto unwritten_before = unwritten_builds;
        const auto resources_before = sample_resources();
        const auto frame_start = Clock::now();
        const auto frame_cpu_start = std::clock();
        {
            pulp::test::RtAllocationProbe probe;
            const auto update_start = Clock::now();
            if (interactive)
                fixture.step(frame);
            laps[0] = Clock::now();
            fixture.root().layout_children();
            laps[1] = Clock::now();
            fixture.root().paint_all(target.begin());
            laps[2] = Clock::now();
            metrics.paint_commands_last = target.end();
            laps[3] = Clock::now();
            allocations = probe.allocation_count();
            allocated_bytes = probe.allocated_bytes();
            if (sampled) {
                update_ms.push_back(elapsed_ms(update_start, laps[0]));
                layout_ms.push_back(elapsed_ms(laps[0], laps[1]));
                record_ms.push_back(elapsed_ms(laps[1], laps[2]));
                submit_ms.push_back(elapsed_ms(laps[2], laps[3]));
            }
        }
        const auto frame_cpu_end = std::clock();
        const auto frame_end = Clock::now();
        const auto resources_after = sample_resources();
        if (interactive) {
            const auto verify_cpu_start = std::clock();
            fixture.verify(frame);
            verify_cpu_ms += cpu_clock_delta_ms(verify_cpu_start, std::clock());
        }
        if (sampled) {
            const bool first = frame == warmup_frames;
            auto& r = metrics.resources;
            const auto& a = resources_after;
            const auto& b = resources_before;
            r.image_decodes.add(a.image_decodes - b.image_decodes, first);
            r.gpu_uploads.add(a.gpu_uploads - b.gpu_uploads, first);
            r.paragraph_builds.add(a.paragraph_builds - b.paragraph_builds, first);
            r.paragraph_builds_unwritten.add(unwritten_builds - unwritten_before, first);
            r.scene_records.add(a.scene_records - b.scene_records, first);
            r.scene_replays.add(a.scene_replays - b.scene_replays, first);
            r.image_cache_hits.add(a.image_cache_hits - b.image_cache_hits, first);
            r.image_cache_builds.add(a.image_cache_builds - b.image_cache_builds, first);
            metrics.paint_commands_total += metrics.paint_commands_last;
            ++metrics.paint_calls;
            const double total = elapsed_ms(frame_start, frame_end);
            frame_ms.push_back(total);
            const double laps_total =
                update_ms.back() + layout_ms.back() + record_ms.back() + submit_ms.back();
            metrics.stages.residual_ms_max =
                std::max(metrics.stages.residual_ms_max, total - laps_total);
            if (total - laps_total >
                std::max(kStageResidualFloorMs, kStageResidualFraction * total))
                ++metrics.stages.frames_over_residual;
            frame_cpu_ms.push_back(cpu_clock_delta_ms(frame_cpu_start, frame_cpu_end));
            frame_allocations.push_back(static_cast<double>(allocations));
            metrics.allocations_total += allocations;
            metrics.allocated_bytes_total += allocated_bytes;
            rss.add(rss_reading());
            footprint.add(phys_footprint_reading());
        }
        ++frame;

        if (frames)
            continue;
        next_frame += interval_duration;
        const auto now = Clock::now();
        if (next_frame > now) std::this_thread::sleep_until(next_frame);
    } while (frames ? frame < *frames : elapsed_ms(phase_start) < static_cast<double>(duration_ms));
    metrics.host_end = sample_host(options.planted_load);

    metrics.samples = static_cast<int>(frame_ms.size());
    metrics.stages_consistent = static_cast<double>(metrics.stages.frames_over_residual) <=
                                kStageResidualFrameShare * static_cast<double>(metrics.samples);
    if (!frame_ms.empty()) {
        metrics.frame_ms_median = percentile(frame_ms, 50.0);
        metrics.frame_ms_p95 = percentile(frame_ms, 95.0);
        metrics.frame_ms_p99 = percentile(frame_ms, 99.0);
        metrics.frame_ms_max = *std::max_element(frame_ms.begin(), frame_ms.end());
    }
    if (!frame_cpu_ms.empty()) {
        metrics.cpu_frame_ms_median = percentile(frame_cpu_ms, 50.0);
        metrics.cpu_frame_ms_p99 = percentile(frame_cpu_ms, 99.0);
    }
    metrics.stages.update = stage_stats(update_ms);
    metrics.stages.layout = stage_stats(layout_ms);
    metrics.stages.record = stage_stats(record_ms);
    metrics.stages.submit = stage_stats(submit_ms);
    if (interactive && !update_ms.empty()) {
        metrics.update_ms_median = percentile(update_ms, 50.0);
        metrics.update_ms_p95 = percentile(update_ms, 95.0);
        metrics.update_ms_p99 = percentile(update_ms, 99.0);
        metrics.update_ms_max = *std::max_element(update_ms.begin(), update_ms.end());
    }
    if (!frame_allocations.empty()) {
        metrics.allocations_per_frame_median = percentile(frame_allocations, 50.0);
        metrics.allocations_per_frame_p95 = percentile(frame_allocations, 95.0);
    }
    metrics.rss = rss.summary();
    metrics.phys_footprint = footprint.summary();
    const auto cpu_end = std::clock();
    // Verification is the benchmark checking itself, not work under test.
    metrics.cpu_ms = std::max(0.0, cpu_clock_delta_ms(cpu_start, cpu_end) - verify_cpu_ms);
    if (counters_from) {
        metrics.bridge_calls = fixture.bridge_call_count() - counters_from->bridge_calls;
        metrics.layout_passes = View::layout_pass_count() - counters_from->layout_passes;
        metrics.invalidation_requests =
            View::damage_request_count() - counters_from->invalidation_requests;
        metrics.unmarked_paint_mutations =
            View::unmarked_paint_mutation_count() - counters_from->unmarked_paint_mutations;
    }
    metrics.views_constructed = View::constructed_count() - constructed_start;
    metrics.views_destroyed = View::destroyed_count() - destroyed_start;
    metrics.slot_identity_changes = fixture.slot_identity_change_count() - identity_start;
    metrics.slot_text_mismatches = fixture.slot_text_mismatch_count() - mismatch_start;
    metrics.js_evaluations = fixture.js_evaluation_count();
    return metrics;
}

// Settled build/run/teardown cycles. The first cycle absorbs one-time
// allocations (font caches, the script engine's runtime) and is excluded from
// the slope; the rest are the steady state, where a correct implementation
// returns to the same footprint after every teardown.
struct CycleMetrics {
    int count = 0;
    int settle_cycles = 1;
    std::vector<Reading> rss_after;
    std::vector<Reading> footprint_after;
    std::optional<double> rss_slope;
    std::optional<double> footprint_slope;
    std::string rss_reason;
    std::string footprint_reason;
    bool planted_leak = false;
};

constexpr std::size_t kPlantedCycleLeakBytes = 4u * 1024u * 1024u;

std::optional<double> settled_slope(const std::vector<Reading>& readings, int settle,
                                    std::string& reason) {
    std::vector<double> values;
    for (std::size_t i = static_cast<std::size_t>(settle); i < readings.size(); ++i) {
        if (!readings[i].value) {
            reason = readings[i].reason;
            return std::nullopt;
        }
        values.push_back(static_cast<double>(*readings[i].value));
    }
    auto slope = least_squares_slope(values);
    if (!slope)
        reason = "fewer than two settled cycles";
    return slope;
}

CycleMetrics run_cycles(const Config& config, int cycles) {
    CycleMetrics metrics;
    metrics.count = cycles;
    metrics.planted_leak = config.plant_cycle_leak;
    // Held for the life of the process: the planted leak must stay resident.
    static std::vector<std::unique_ptr<char[]>> planted;
    const int frames = config.frames.value_or(60);
    for (int cycle = 0; cycle < cycles; ++cycle) {
        {
            auto fixture = make_fixture(config);
            run_phase(*fixture, 0, frames, config.target_fps, true);
        }
        if (config.plant_cycle_leak) {
            planted.push_back(std::make_unique<char[]>(kPlantedCycleLeakBytes));
            // Touch every page so the leak is resident, not merely reserved.
            std::fill_n(planted.back().get(), kPlantedCycleLeakBytes, char{1});
        }
        metrics.rss_after.push_back(rss_reading());
        metrics.footprint_after.push_back(phys_footprint_reading());
    }
    metrics.rss_slope = settled_slope(metrics.rss_after, metrics.settle_cycles, metrics.rss_reason);
    metrics.footprint_slope =
        settled_slope(metrics.footprint_after, metrics.settle_cycles, metrics.footprint_reason);
    return metrics;
}

bool write_file(const std::filesystem::path& path, const std::string& text) {
    if (path.empty()) return true;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path);
    if (!out.is_open()) return false;
    out << text;
    return out.good();
}

// A JSON number, or null for an absent value; never a stand-in 0.
template <typename T> std::string json_number_or_null(const std::optional<T>& value) {
    if (!value)
        return "null";
    std::ostringstream out;
    out << std::fixed << std::setprecision(4) << *value;
    return out.str();
}

std::string json_string_or_null(const std::string& value) {
    return value.empty() ? "null" : "\"" + json_escape(value) + "\"";
}

std::string reading_json(const Reading& reading) {
    std::ostringstream out;
    out << "{\"value\": " << (reading.value ? std::to_string(*reading.value) : std::string("null"))
        << ", \"source\": " << json_string_or_null(reading.source)
        << ", \"reason\": " << json_string_or_null(reading.reason) << "}";
    return out.str();
}

std::string memory_json(const MemoryStats& stats) {
    const auto bytes = [](const std::optional<std::uint64_t>& value) {
        return value ? std::to_string(*value) : std::string("null");
    };
    std::ostringstream out;
    out << "{\"median_bytes\": " << bytes(stats.median_bytes)
        << ", \"p99_bytes\": " << bytes(stats.p99_bytes)
        << ", \"peak_bytes\": " << bytes(stats.peak_bytes)
        << ", \"source\": " << json_string_or_null(stats.source)
        << ", \"reason\": " << json_string_or_null(stats.reason) << "}";
    return out.str();
}

std::string host_sample_json(const std::optional<HostSample>& sample) {
    if (!sample)
        return "null";
    std::ostringstream out;
    out << "{\"load_1m\": " << json_number_or_null(sample->load_1m)
        << ", \"load_reason\": " << json_string_or_null(sample->load_reason)
        << ", \"thermal_state\": " << json_string_or_null(sample->thermal)
        << ", \"thermal_reason\": " << json_string_or_null(sample->thermal_reason) << "}";
    return out.str();
}

std::string stage_json(const StageStats& stats) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(4) << "{\"median_ms\": " << stats.median_ms
        << ", \"p95_ms\": " << stats.p95_ms << ", \"max_ms\": " << stats.max_ms << "}";
    return out.str();
}

// Steady state does no resource work: after a phase's first frame, no frame
// may decode an image, upload one to the GPU or rebuild an image-cache entry,
// and no frame may shape more text runs than its update wrote. A target that
// never decodes, uploads or shapes cannot fail this gate, so its verdict is
// `blind`, not `pass`.
struct SteadyStateGate {
    std::string status;
    std::vector<std::string> failures;
};

SteadyStateGate steady_state_gate(const PhaseMetrics& idle, const PhaseMetrics& interactive,
                                  bool observes_resources) {
    SteadyStateGate gate;
    if (!observes_resources) {
        gate.status = "blind";
        return gate;
    }
    for (const auto& [name, phase] : {std::pair<const char*, const PhaseMetrics*>{"idle", &idle},
                                      {"interactive", &interactive}}) {
        const auto check = [&](const char* what, const ResourceSeries& series) {
            if (series.frames_after_first_with_work != 0)
                gate.failures.push_back(std::string(name) + ": " +
                                        std::to_string(series.frames_after_first_with_work) +
                                        " frames after the first did " + what);
        };
        check("image decodes", phase->resources.image_decodes);
        check("GPU uploads", phase->resources.gpu_uploads);
        check("image-cache builds", phase->resources.image_cache_builds);
        check("paragraph builds of text the update did not write",
              phase->resources.paragraph_builds_unwritten);
    }
    gate.status = gate.failures.empty() ? "pass" : "fail";
    return gate;
}

std::string resource_json(const ResourceSeries& series) {
    return "{\"total\": " + std::to_string(series.total) +
           ", \"per_frame_max\": " + std::to_string(series.per_frame_max) +
           ", \"frames_after_first_with_work\": " +
           std::to_string(series.frames_after_first_with_work) + "}";
}

// Paragraph builds also carry the subset the gate judges: builds of text the
// frame's update did not write.
std::string paragraph_builds_json(const PhaseResources& r) {
    auto json = resource_json(r.paragraph_builds);
    json.pop_back();
    return json + ", \"unwritten\": " + resource_json(r.paragraph_builds_unwritten) + "}";
}

void append_phase_json(std::ostringstream& out,
                       std::string_view name,
                       const PhaseMetrics& metrics,
                       bool trailing_comma) {
    out << "  \"" << name << "\": {\n";
    out << "    \"duration_ms\": " << metrics.duration_ms << ",\n";
    out << "    \"samples\": " << metrics.samples << ",\n";
    out << "    \"warmup_frames\": " << metrics.warmup_frames << ",\n";
    out << "    \"cpu_ms\": " << metrics.cpu_ms << ",\n";
    out << "    \"cpu_frame_ms_median\": " << metrics.cpu_frame_ms_median << ",\n";
    out << "    \"cpu_frame_ms_p99\": " << metrics.cpu_frame_ms_p99 << ",\n";
    out << "    \"frame_ms_median\": " << metrics.frame_ms_median << ",\n";
    out << "    \"frame_ms_p95\": " << metrics.frame_ms_p95 << ",\n";
    out << "    \"frame_ms_p99\": " << metrics.frame_ms_p99 << ",\n";
    out << "    \"frame_ms_max\": " << metrics.frame_ms_max << ",\n";
    out << "    \"update_ms_median\": " << metrics.update_ms_median << ",\n";
    out << "    \"update_ms_p95\": " << metrics.update_ms_p95 << ",\n";
    out << "    \"update_ms_p99\": " << metrics.update_ms_p99 << ",\n";
    out << "    \"update_ms_max\": " << metrics.update_ms_max << ",\n";
    out << "    \"stages\": {\n";
    out << "      \"update\": " << stage_json(metrics.stages.update) << ",\n";
    out << "      \"layout\": " << stage_json(metrics.stages.layout) << ",\n";
    out << "      \"record\": " << stage_json(metrics.stages.record) << ",\n";
    out << "      \"submit\": " << stage_json(metrics.stages.submit) << ",\n";
    out << "      \"residual_ms_max\": " << metrics.stages.residual_ms_max << ",\n";
    out << "      \"frames_over_residual\": " << metrics.stages.frames_over_residual << ",\n";
    out << "      \"consistent\": " << (metrics.stages_consistent ? "true" : "false") << "\n";
    out << "    },\n";
    out << "    \"memory\": {\n";
    out << "      \"rss\": " << memory_json(metrics.rss) << ",\n";
    out << "      \"phys_footprint\": " << memory_json(metrics.phys_footprint) << "\n";
    out << "    },\n";
    out << "    \"host\": {\"start\": " << host_sample_json(metrics.host_start)
        << ", \"end\": " << host_sample_json(metrics.host_end) << "},\n";
    out << "    \"allocations_total\": " << metrics.allocations_total << ",\n";
    out << "    \"allocated_bytes_total\": " << metrics.allocated_bytes_total << ",\n";
    out << "    \"allocations_per_frame_median\": " << metrics.allocations_per_frame_median
        << ",\n";
    out << "    \"allocations_per_frame_p95\": " << metrics.allocations_per_frame_p95 << ",\n";
    out << "    \"paint_commands_last\": " << metrics.paint_commands_last << ",\n";
    out << "    \"js_evaluations_total\": " << metrics.js_evaluations << ",\n";
    out << "    \"counters\": {\n";
    out << "      \"counter_schema\": \"pulp-design-import-benchmark-counters-v2\",\n";
    out << "      \"bridge_calls\": " << metrics.bridge_calls << ",\n";
    out << "      \"layout_passes\": " << metrics.layout_passes << ",\n";
    out << "      \"paint_calls\": " << metrics.paint_calls << ",\n";
    out << "      \"paint_commands_total\": " << metrics.paint_commands_total << ",\n";
    out << "      \"invalidation_requests\": " << metrics.invalidation_requests << ",\n";
    out << "      \"unmarked_paint_mutations\": " << metrics.unmarked_paint_mutations << ",\n";
    out << "      \"views_constructed\": " << metrics.views_constructed << ",\n";
    out << "      \"views_destroyed\": " << metrics.views_destroyed << ",\n";
    out << "      \"slot_identity_changes\": " << metrics.slot_identity_changes << ",\n";
    out << "      \"slot_text_mismatches\": " << metrics.slot_text_mismatches << ",\n";
    const auto& r = metrics.resources;
    out << "      \"resources\": {\n";
    out << "        \"image_decodes\": " << resource_json(r.image_decodes) << ",\n";
    out << "        \"gpu_uploads\": " << resource_json(r.gpu_uploads) << ",\n";
    out << "        \"paragraph_builds\": " << paragraph_builds_json(r) << ",\n";
    out << "        \"scene_cache\": {\"records\": " << resource_json(r.scene_records)
        << ", \"replays\": " << resource_json(r.scene_replays) << "},\n";
    out << "        \"image_cache\": {\"hits\": " << resource_json(r.image_cache_hits)
        << ", \"builds\": " << resource_json(r.image_cache_builds) << "}\n";
    out << "      }\n";
    out << "    }\n";
    out << "  }" << (trailing_comma ? "," : "") << "\n";
}

// Views alive after the fixture is destroyed: the terminal lifetime readback.
struct TeardownMetrics {
    std::uint64_t views_constructed = 0;
    std::uint64_t views_destroyed = 0;
    std::uint64_t live_views_after_teardown = 0;
    bool planted_leak = false;
};

struct IrProvenance {
    std::string file_name;
    std::string sha256;
    std::size_t slots = 0;
    std::vector<std::string> materialization_warnings;
    // The materialized runtime bundle the live lane mounted, if any.
    std::string bundle_file_name;
    std::string bundle_sha256;
};

// Receipt fields that describe the run rather than one phase.
struct RunContext {
    Reading rss_high_water;
    HostConditions host;
    std::optional<double> planted_load;
    std::optional<CycleMetrics> cycles;
    std::string canvas = "recording";
    SteadyStateGate steady_state{"blind", {}};
    bool planted_frame_decode = false;
    bool paragraph_cache = true;
    std::uint64_t paint_commands_recorded = 0;
};

std::string cycles_json(const CycleMetrics& cycles) {
    std::ostringstream out;
    out << "{\n";
    out << "    \"count\": " << cycles.count << ",\n";
    out << "    \"settle_cycles\": " << cycles.settle_cycles << ",\n";
    out << "    \"planted_leak\": " << (cycles.planted_leak ? "true" : "false") << ",\n";
    out << "    \"rss_after\": [";
    for (std::size_t i = 0; i < cycles.rss_after.size(); ++i)
        out << (i ? ", " : "") << reading_json(cycles.rss_after[i]);
    out << "],\n";
    out << "    \"phys_footprint_after\": [";
    for (std::size_t i = 0; i < cycles.footprint_after.size(); ++i)
        out << (i ? ", " : "") << reading_json(cycles.footprint_after[i]);
    out << "],\n";
    out << "    \"rss_slope_bytes_per_cycle\": " << json_number_or_null(cycles.rss_slope) << ",\n";
    out << "    \"rss_slope_reason\": " << json_string_or_null(cycles.rss_reason) << ",\n";
    out << "    \"phys_footprint_slope_bytes_per_cycle\": "
        << json_number_or_null(cycles.footprint_slope) << ",\n";
    out << "    \"phys_footprint_slope_reason\": " << json_string_or_null(cycles.footprint_reason)
        << "\n";
    out << "  }";
    return out.str();
}

std::string make_json(const Config& config, const std::optional<IrProvenance>& ir,
                      const StartupMetrics& startup, const PhaseMetrics& idle,
                      const PhaseMetrics& interactive, const TeardownMetrics& teardown,
                      const RunContext& run = {}) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(4);
    out << "{\n";
    out << "  \"schema\": \"pulp-design-import-benchmark-v2\",\n";
    out << "  \"lane\": \"" << json_escape(config.lane) << "\",\n";
    out << "  \"canvas\": \"" << json_escape(run.canvas) << "\",\n";
    out << "  \"fixture\": \"" << (ir ? json_escape(ir->file_name) : std::string(kFixtureName))
        << "\",\n";
    if (ir) {
        out << "  \"ir\": {\n";
        out << "    \"sha256\": \"" << ir->sha256 << "\",\n";
        out << "    \"text_slots\": " << ir->slots << ",\n";
        out << "    \"materialization_warnings\": [";
        for (std::size_t i = 0; i < ir->materialization_warnings.size(); ++i)
            out << (i ? ", " : "") << "\"" << json_escape(ir->materialization_warnings[i]) << "\"";
        out << "],\n";
        out << "    \"bundle\": "
            << (ir->bundle_sha256.empty() ? std::string("null")
                                          : "{\"file\": \"" + json_escape(ir->bundle_file_name) +
                                                "\", \"sha256\": \"" + ir->bundle_sha256 + "\"}")
            << "\n";
        out << "  },\n";
    }
    out << "  \"pacing\": \"" << (config.frames ? "fixed-frames" : "wall-clock") << "\",\n";
    if (config.frames)
        out << "  \"frames_per_phase\": " << *config.frames << ",\n";
    out << "  \"host\": \"" << json_escape(host_name()) << "\",\n";
    out << "  \"platform\": \"" << json_escape(platform_name()) << "\",\n";
    out << "  \"host_conditions\": {\n";
    out << "    \"cores\": " << run.host.cores << ",\n";
    out << "    \"load_threshold\": " << run.host.load_threshold << ",\n";
    out << "    \"planted_load\": " << json_number_or_null(run.planted_load) << ",\n";
    out << "    \"confounded\": " << (run.host.confounded ? "true" : "false") << ",\n";
    out << "    \"confounders\": [";
    for (std::size_t i = 0; i < run.host.confounders.size(); ++i)
        out << (i ? ", " : "") << "\"" << json_escape(run.host.confounders[i]) << "\"";
    out << "]\n";
    out << "  },\n";
    out << "  \"target_fps\": " << config.target_fps << ",\n";
    out << "  \"startup\": {\n";
    out << "    \"build_ms\": " << startup.build_ms << ",\n";
    out << "    \"first_frame_ms\": " << startup.first_frame_ms << ",\n";
    out << "    \"first_frame_render_ms\": " << startup.first_frame_render_ms << ",\n";
    out << "    \"first_frame_paint_commands\": " << startup.first_frame_paint_commands << ",\n";
    out << "    \"rss_after_first_frame\": " << reading_json(startup.rss_after_first_frame) << "\n";
    out << "  },\n";
    out << "  \"rss_high_water\": " << reading_json(run.rss_high_water) << ",\n";
    out << "  \"paint_commands_recorded\": " << run.paint_commands_recorded << ",\n";
    out << "  \"interaction_model\": \"value drag plus overflow-panel scroll offset; live lane "
           "mutates through JS bridge, baked-native materializes once and uses direct native "
           "setters, baked-native-rebuild rematerializes the same IR per interactive step, "
           "baked-cpp mutates generated-style native widgets directly; with --ir every lane "
           "writes the IR's anchored text slots instead\",\n";
    out << "  \"paragraph_cache\": " << (run.paragraph_cache ? "true" : "false") << ",\n";
    out << "  \"steady_state_gate\": {\n";
    out << "    \"status\": \"" << run.steady_state.status << "\",\n";
    out << "    \"planted_frame_decode\": " << (run.planted_frame_decode ? "true" : "false")
        << ",\n";
    out << "    \"failures\": [";
    for (std::size_t i = 0; i < run.steady_state.failures.size(); ++i)
        out << (i ? ", " : "") << "\"" << json_escape(run.steady_state.failures[i]) << "\"";
    out << "]\n";
    out << "  },\n";
    append_phase_json(out, "idle", idle, true);
    append_phase_json(out, "interactive", interactive, true);
    if (run.cycles)
        out << "  \"cycles\": " << cycles_json(*run.cycles) << ",\n";
    out << "  \"teardown\": {\n";
    out << "    \"views_constructed\": " << teardown.views_constructed << ",\n";
    out << "    \"views_destroyed\": " << teardown.views_destroyed << ",\n";
    out << "    \"live_views_after_teardown\": " << teardown.live_views_after_teardown << ",\n";
    out << "    \"planted_leak\": " << (teardown.planted_leak ? "true" : "false") << "\n";
    out << "  }\n";
    out << "}\n";
    return out.str();
}

void print_usage(std::ostream& out) {
    out << "usage: pulp-design-import-bench "
           "[--lane=live|baked-native|baked-native-rebuild|baked-cpp] "
           "[--idle-ms=N] [--interactive-ms=N] [--target-fps=N] [--warmup-frames=N] "
           "[--output=PATH] "
           "[--ir=DESIGN_IR_JSON] [--bundle=MATERIALIZED_BUNDLE_JS] [--frames=N] "
           "[--cycles=N] [--canvas=recording|skia-raster|skia-graphite] "
           "[--require-steady-state] [--plant-leak] [--plant-cycle-leak] [--plant-load=LOAD] "
           "[--plant-frame-decode] [--paragraph-cache=on|off]\n"
           "  --ir measures an importer-produced DesignIR with anchored text nodes as\n"
           "     keyed slots (live, baked-native and baked-native-rebuild).\n"
           "  --bundle mounts the materialized runtime bundle built from the same\n"
           "     source under the live lane's DesignIR (requires --lane=live --ir).\n"
           "  --frames runs exactly N frames per phase without pacing, so every\n"
           "     counter in the receipt is reproducible.\n"
           "  --cycles runs N >= 3 build/run/teardown cycles after the phases and\n"
           "     reports the memory slope across the settled ones.\n"
           "  --canvas paints every frame into a RecordingCanvas (default) or through\n"
           "     SkiaCanvas into a CPU raster or headless Graphite surface, where\n"
           "     decodes, uploads and text shaping are measured.\n"
           "  --require-steady-state exits 4 when a frame after a phase's first\n"
           "     decodes, uploads or rebuilds an image, or shapes text its update\n"
           "     did not write.\n"
           "  --plant-leak leaks one view past teardown; the run must then fail.\n"
           "  --plant-cycle-leak leaks 4 MiB per cycle; the slope must then rise.\n"
           "  --plant-load reports LOAD as the one-minute load average; a load\n"
           "     above cores/4 must mark the receipt confounded.\n"
           "  --plant-frame-decode adds a view that decodes an image every paint;\n"
           "     the steady-state gate must then fail.\n"
           "  --paragraph-cache=off shapes every text run on every paint, the\n"
           "     uncached text path; the steady-state gate must then fail on text.\n";
}

bool parse_double(std::string_view value, double& out) {
    try {
        std::size_t used = 0;
        const double parsed = std::stod(std::string(value), &used);
        if (used != value.size() || !std::isfinite(parsed))
            return false;
        out = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_int(std::string_view value, int& out) {
    try {
        std::size_t used = 0;
        int parsed = std::stoi(std::string(value), &used);
        if (used != value.size()) return false;
        out = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

std::optional<Config> parse_args(int argc, char** argv) {
    Config config;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto value_after = [&](std::string_view prefix) -> std::optional<std::string> {
            if (arg.rfind(prefix, 0) == 0) return arg.substr(prefix.size());
            return std::nullopt;
        };

        if (arg == "--help" || arg == "-h") {
            print_usage(std::cout);
            std::exit(0);
        } else if (auto value = value_after("--lane=")) {
            config.lane = *value;
        } else if (arg == "--lane" && i + 1 < argc) {
            config.lane = argv[++i];
        } else if (auto value = value_after("--idle-ms=")) {
            if (!parse_int(*value, config.idle_ms)) return std::nullopt;
        } else if (arg == "--idle-ms" && i + 1 < argc) {
            if (!parse_int(argv[++i], config.idle_ms)) return std::nullopt;
        } else if (auto value = value_after("--interactive-ms=")) {
            if (!parse_int(*value, config.interactive_ms)) return std::nullopt;
        } else if (arg == "--interactive-ms" && i + 1 < argc) {
            if (!parse_int(argv[++i], config.interactive_ms)) return std::nullopt;
        } else if (auto value = value_after("--target-fps=")) {
            if (!parse_int(*value, config.target_fps)) return std::nullopt;
        } else if (arg == "--target-fps" && i + 1 < argc) {
            if (!parse_int(argv[++i], config.target_fps)) return std::nullopt;
        } else if (auto value = value_after("--warmup-frames=")) {
            if (!parse_int(*value, config.warmup_frames))
                return std::nullopt;
        } else if (arg == "--warmup-frames" && i + 1 < argc) {
            if (!parse_int(argv[++i], config.warmup_frames))
                return std::nullopt;
        } else if (auto value = value_after("--ir=")) {
            config.ir_path = *value;
        } else if (arg == "--ir" && i + 1 < argc) {
            config.ir_path = argv[++i];
        } else if (auto value = value_after("--frames=")) {
            int frames = 0;
            if (!parse_int(*value, frames) || frames < 0)
                return std::nullopt;
            config.frames = frames;
        } else if (auto value = value_after("--bundle=")) {
            config.bundle_path = *value;
        } else if (arg == "--bundle" && i + 1 < argc) {
            config.bundle_path = argv[++i];
        } else if (auto value = value_after("--cycles=")) {
            int cycles = 0;
            if (!parse_int(*value, cycles))
                return std::nullopt;
            config.cycles = cycles;
        } else if (auto value = value_after("--plant-load=")) {
            double load = 0.0;
            if (!parse_double(*value, load) || load < 0.0)
                return std::nullopt;
            config.plant_load = load;
        } else if (auto value = value_after("--canvas=")) {
            config.canvas = *value;
        } else if (arg == "--canvas" && i + 1 < argc) {
            config.canvas = argv[++i];
        } else if (arg == "--require-steady-state") {
            config.require_steady_state = true;
        } else if (arg == "--plant-frame-decode") {
            config.plant_frame_decode = true;
        } else if (auto value = value_after("--paragraph-cache=")) {
            if (*value != "on" && *value != "off")
                return std::nullopt;
            config.paragraph_cache = *value == "on";
        } else if (arg == "--plant-leak") {
            config.plant_leak = true;
        } else if (arg == "--plant-cycle-leak") {
            config.plant_cycle_leak = true;
        } else if (auto value = value_after("--output=")) {
            config.output_path = *value;
        } else if (arg == "--output" && i + 1 < argc) {
            config.output_path = argv[++i];
        } else {
            return std::nullopt;
        }
    }
    if (config.lane != "live" && config.lane != "baked-native" &&
        config.lane != "baked-native-rebuild" && config.lane != "baked-cpp")
        return std::nullopt;
    // baked-cpp is compiled code; it cannot be built from a DesignIR at run
    // time, so it cannot claim to have measured one.
    if (!config.ir_path.empty() && config.lane == "baked-cpp") {
        std::cerr << "--ir applies to live, baked-native and baked-native-rebuild; the "
                  << config.lane << " lane cannot be built from a DesignIR\n";
        return std::nullopt;
    }
    // A bundle is the behavior realm of a DesignIR the live lane mounts; it
    // means nothing on its own or to a natively materialized lane.
    if (!config.bundle_path.empty() && (config.lane != "live" || config.ir_path.empty())) {
        std::cerr << "--bundle requires --lane=live and the --ir it was built with\n";
        return std::nullopt;
    }
    if (config.canvas != "recording" && config.canvas != "skia-raster" &&
        config.canvas != "skia-graphite") {
        std::cerr << "--canvas must be recording, skia-raster or skia-graphite\n";
        return std::nullopt;
    }
    // A rebuild lane replaces its root every step, which would drop the plant.
    if (config.plant_frame_decode && config.lane == "baked-native-rebuild") {
        std::cerr << "--plant-frame-decode needs a lane that keeps its root\n";
        return std::nullopt;
    }
    // A slope needs two settled points after the cycle that absorbs one-time
    // allocations; fewer cycles cannot distinguish growth from noise.
    if (config.cycles && *config.cycles < 3) {
        std::cerr << "--cycles must be at least 3 (one settling cycle plus two measured)\n";
        return std::nullopt;
    }
    config.idle_ms = std::max(0, config.idle_ms);
    config.interactive_ms = std::max(0, config.interactive_ms);
    config.target_fps = std::max(1, config.target_fps);
    config.warmup_frames = std::max(0, config.warmup_frames);
    return config;
}

}  // namespace

int main(int argc, char** argv) {
    const auto process_start = Clock::now();
    auto config = parse_args(argc, argv);
    if (!config) {
        print_usage(std::cerr);
        return 2;
    }

    set_paragraph_cache_enabled(config->paragraph_cache);
    const auto views_constructed_start = View::constructed_count();
    const auto views_destroyed_start = View::destroyed_count();
    const auto build_start = Clock::now();
    std::unique_ptr<BenchmarkFixture> fixture;
    try {
        fixture = make_fixture(*config);
    } catch (const std::exception& e) {
        std::cerr << "benchmark setup failed: " << e.what() << "\n";
        return 1;
    }
    if (!fixture) {
        std::cerr << "unknown lane: " << config->lane << "\n";
        return 2;
    }
    const auto build_end = Clock::now();
    if (config->plant_frame_decode)
        fixture->root().add_child(std::make_unique<PlantedDecodeView>());

    std::string target_error;
    const auto bounds = fixture->root().bounds();
    auto target =
        make_frame_target(config->canvas, static_cast<std::uint32_t>(std::max(1.0f, bounds.width)),
                          static_cast<std::uint32_t>(std::max(1.0f, bounds.height)), target_error);
    if (!target) {
        std::cerr << "--canvas=" << config->canvas << " is unavailable: " << target_error << "\n";
        return 2;
    }
    StartupMetrics startup;
    PhaseMetrics idle;
    PhaseMetrics interactive;
    std::uint64_t paint_commands_recorded = 0;
    // A frame target that loses its device mid-run throws; the run then has
    // no receipt rather than a partial one.
    try {
        const auto first_render_start = Clock::now();
        const auto first_commands = render_frame(fixture->root(), *target);
        const auto first_render_end = Clock::now();

        startup.build_ms = elapsed_ms(build_start, build_end);
        startup.first_frame_render_ms = elapsed_ms(first_render_start, first_render_end);
        startup.first_frame_ms = elapsed_ms(process_start, first_render_end);
        startup.first_frame_paint_commands = first_commands;
        startup.rss_after_first_frame = rss_reading();

        PhaseOptions phase_options;
        phase_options.target = target.get();
        phase_options.planted_load = config->plant_load;
        idle = run_phase(*fixture, config->idle_ms, config->frames, config->target_fps, false, 0,
                         phase_options);
        interactive = run_phase(*fixture, config->interactive_ms, config->frames,
                                config->target_fps, true, config->warmup_frames, phase_options);
        // A Skia target cannot count paint commands, so every receipt also
        // records the final tree once into a RecordingCanvas, untimed. Two
        // lanes that claim to paint the same tree must agree on this count.
        RecordingTarget reference;
        paint_commands_recorded = render_frame(fixture->root(), reference);
    } catch (const std::exception& e) {
        std::cerr << "benchmark run failed: " << e.what() << "\n";
        return 1;
    }

    std::optional<IrProvenance> ir;
    if (auto* bound = dynamic_cast<IrBoundFixture*>(fixture.get()))
        ir = IrProvenance{bound->loaded().file_name,
                          bound->loaded().sha256,
                          bound->loaded().slot_anchors.size(),
                          bound->warning_codes(),
                          {},
                          {}};
    if (auto* live = dynamic_cast<IrLiveFixture*>(fixture.get()))
        ir = IrProvenance{live->loaded().file_name,           live->loaded().sha256,
                          live->loaded().slot_anchors.size(), {},
                          live->bundle_file_name(),           live->bundle_sha256()};

    // Terminal readback: destroy the fixture, then count the views it left.
    TeardownMetrics teardown;
    teardown.planted_leak = config->plant_leak;
    // The planted subtree outlives the fixture but is freed after the
    // readback, so the negative control is a live view, not a real leak.
    std::unique_ptr<View> planted;
    if (config->plant_leak) {
        if (fixture->root().child_count() == 0) {
            std::cerr << "--plant-leak: the fixture root has no child to hold past teardown\n";
            return 2;
        }
        planted = fixture->root().remove_child(fixture->root().child_at(0));
    }
    fixture.reset();
    teardown.views_constructed = View::constructed_count() - views_constructed_start;
    teardown.views_destroyed = View::destroyed_count() - views_destroyed_start;
    teardown.live_views_after_teardown =
        teardown.views_constructed - std::min(teardown.views_constructed, teardown.views_destroyed);
    planted.reset();

    RunContext run;
    std::vector<HostSample> samples;
    for (const auto* phase : {&idle, &interactive}) {
        if (phase->host_start)
            samples.push_back(*phase->host_start);
        if (phase->host_end)
            samples.push_back(*phase->host_end);
    }
    if (samples.empty())
        samples.push_back(sample_host(config->plant_load));
    if (config->cycles) {
        try {
            run.cycles = run_cycles(*config, *config->cycles);
        } catch (const std::exception& e) {
            std::cerr << "benchmark cycles failed: " << e.what() << "\n";
            return 1;
        }
        samples.push_back(sample_host(config->plant_load));
    }
    run.host = classify_host(samples, logical_cores());
    run.planted_load = config->plant_load;
    run.rss_high_water = rss_high_water_reading();
    run.canvas = target->name();
    run.steady_state = steady_state_gate(idle, interactive, target->observes_resources());
    run.planted_frame_decode = config->plant_frame_decode;
    run.paragraph_cache = config->paragraph_cache;
    run.paint_commands_recorded = paint_commands_recorded;
    target.reset();
    const auto json = make_json(*config, ir, startup, idle, interactive, teardown, run);

    if (!config->output_path.empty()) {
        if (!write_file(config->output_path, json)) {
            std::cerr << "failed to write " << config->output_path << "\n";
            return 1;
        }
    } else {
        std::cout << json;
    }
    if (teardown.live_views_after_teardown != 0) {
        std::cerr << "teardown left " << teardown.live_views_after_teardown << " live views\n";
        return 3;
    }
    if (config->require_steady_state && run.steady_state.status != "pass") {
        std::cerr << "steady-state resource gate: " << run.steady_state.status << "\n";
        for (const auto& failure : run.steady_state.failures)
            std::cerr << "  " << failure << "\n";
        return 4;
    }
    return 0;
}
