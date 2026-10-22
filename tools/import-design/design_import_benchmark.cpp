#include <pulp/canvas/canvas.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/design_ir_io.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/widgets.hpp>

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
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <unistd.h>
#elif defined(__linux__)
#include <unistd.h>
#include <fstream>
#endif

using namespace pulp::view;

namespace {

constexpr int kFixtureWidth = 760;
constexpr int kFixtureHeight = 420;
constexpr const char* kFixtureName = "phase5-imported-plugin-panel";

using Clock = std::chrono::steady_clock;

struct Config {
    std::string lane = "live";
    int idle_ms = 60000;
    int interactive_ms = 60000;
    int target_fps = 60;
    std::filesystem::path output_path;
    // An importer-produced DesignIR to measure instead of the built-in panel.
    std::filesystem::path ir_path;
    // When set, each phase runs exactly this many frames without pacing, so
    // every counter in the receipt is reproducible run to run.
    std::optional<int> frames;
    // Planted negative: leak one view past teardown, to prove the terminal
    // readback can fail.
    bool plant_leak = false;
};

struct PhaseMetrics {
    int duration_ms = 0;
    int samples = 0;
    double cpu_ms = 0.0;
    double cpu_frame_ms_median = 0.0;
    double cpu_frame_ms_p99 = 0.0;
    double frame_ms_median = 0.0;
    double frame_ms_p99 = 0.0;
    double frame_ms_max = 0.0;
    double update_ms_median = 0.0;
    double update_ms_p99 = 0.0;
    double update_ms_max = 0.0;
    std::uint64_t rss_median_bytes = 0;
    std::uint64_t rss_p99_bytes = 0;
    std::uint64_t rss_peak_bytes = 0;
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
};

struct StartupMetrics {
    double build_ms = 0.0;
    double first_frame_ms = 0.0;
    double first_frame_render_ms = 0.0;
    std::uint64_t first_frame_paint_commands = 0;
    std::uint64_t rss_after_first_frame_bytes = 0;
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

double percentile(std::vector<double> values, double pct) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const double rank = (pct / 100.0) * static_cast<double>(values.size() - 1);
    const auto lo = static_cast<std::size_t>(std::floor(rank));
    const auto hi = static_cast<std::size_t>(std::ceil(rank));
    if (lo == hi) return values[lo];
    const double w = rank - static_cast<double>(lo);
    return values[lo] * (1.0 - w) + values[hi] * w;
}

std::uint64_t current_rss_bytes() {
#if defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(),
                  MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info),
                  &count) == KERN_SUCCESS) {
        return static_cast<std::uint64_t>(info.resident_size);
    }
    return 0;
#elif defined(__linux__)
    std::ifstream statm("/proc/self/statm");
    std::uint64_t pages = 0;
    std::uint64_t resident = 0;
    if (statm >> pages >> resident) {
        const long page_size = sysconf(_SC_PAGESIZE);
        if (page_size > 0) return resident * static_cast<std::uint64_t>(page_size);
    }
    return 0;
#else
    return 0;
#endif
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

class BenchmarkFixture {
public:
    virtual ~BenchmarkFixture() = default;
    virtual View& root() = 0;
    virtual void step(int frame) = 0;
    virtual int js_evaluation_count() const = 0;
    virtual std::uint64_t bridge_call_count() const {
        return 0;
    }
    // Anchor-keyed text slots whose View changed identity across an update,
    // and slots whose text did not read back as written. Only fixtures that
    // drive keyed slots report non-zero.
    virtual std::uint64_t slot_identity_change_count() const { return 0; }
    virtual std::uint64_t slot_text_mismatch_count() const { return 0; }
};

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
        root_->layout_children();
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
        root_->layout_children();
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
  layout();
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

View* find_by_anchor(View& root, std::string_view anchor) {
    if (root.anchor_id() == anchor) return &root;
    for (std::size_t i = 0; i < root.child_count(); ++i)
        if (auto* found = find_by_anchor(*root.child_at(i), anchor)) return found;
    return nullptr;
}

// An importer-produced DesignIR, with the provenance that binds a receipt to
// the exact bytes measured.
struct LoadedIr {
    DesignIR ir;
    std::string sha256;
    std::string file_name;
    float width = static_cast<float>(kFixtureWidth);
    float height = static_cast<float>(kFixtureHeight);
    std::vector<std::string> slot_anchors;
};

LoadedIr load_ir(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read DesignIR " + path.string());
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    LoadedIr loaded;
    loaded.ir = parse_design_ir_json(text);
    loaded.sha256 = pulp::runtime::sha256_hex(text);
    loaded.file_name = path.filename().string();
    if (loaded.ir.root.style.width && *loaded.ir.root.style.width > 0.0f)
        loaded.width = *loaded.ir.root.style.width;
    if (loaded.ir.root.style.height && *loaded.ir.root.style.height > 0.0f)
        loaded.height = *loaded.ir.root.style.height;
    const auto collect = [&](auto& self, const IRNode& node) -> void {
        if (node.type == "text" && !node.text_content.empty() && node.stable_anchor_id &&
            !node.stable_anchor_id->empty())
            loaded.slot_anchors.push_back(*node.stable_anchor_id);
        for (const auto& child : node.children) self(self, child);
    };
    collect(collect, loaded.ir.root);
    if (loaded.slot_anchors.empty())
        throw std::runtime_error("DesignIR " + loaded.file_name + " has no anchored text slots");
    return loaded;
}

std::string slot_text(std::size_t slot, int frame) {
    return "slot " + std::to_string(slot) + " frame " + std::to_string(frame);
}

// The retained-plan experiment over real importer output. Each anchored text
// node is a typed slot. The retained lane resolves every slot to its Label
// once and writes through it; the rebuild lane is the control, writing the
// same text into a copy of the IR and rematerializing the whole tree. Both
// read every slot back by anchor after the update, so a slot that rebound to
// a different View, or kept stale text, is counted rather than assumed away.
class IrBoundFixture final : public BenchmarkFixture {
  public:
    IrBoundFixture(const std::filesystem::path& path, bool rebuild_each_step)
        : loaded_(load_ir(path)), rebuild_each_step_(rebuild_each_step) {
        materialize(loaded_.ir);
        for (const auto& anchor : loaded_.slot_anchors) {
            auto* label = dynamic_cast<Label*>(find_by_anchor(*root_, anchor));
            if (!label)
                throw std::runtime_error("text slot " + anchor + " did not materialize as a Label");
            slots_.push_back({label, label->import_binding_instance_id()});
        }
    }

    View& root() override { return *root_; }
    int js_evaluation_count() const override { return 0; }
    std::uint64_t slot_identity_change_count() const override { return identity_changes_; }
    std::uint64_t slot_text_mismatch_count() const override { return text_mismatches_; }
    const LoadedIr& loaded() const { return loaded_; }

    void step(int frame) override {
        if (rebuild_each_step_) {
            auto ir = loaded_.ir;
            std::size_t slot = 0;
            const auto write = [&](auto& self, IRNode& node) -> void {
                if (node.type == "text" && node.stable_anchor_id &&
                    slot < loaded_.slot_anchors.size() &&
                    *node.stable_anchor_id == loaded_.slot_anchors[slot])
                    node.text_content = slot_text(slot++, frame);
                for (auto& child : node.children) self(self, child);
            };
            write(write, ir.root);
            materialize(ir);
        } else {
            for (std::size_t i = 0; i < slots_.size(); ++i)
                slots_[i].label->set_text(slot_text(i, frame));
            root_->layout_children();
        }
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            auto* label = dynamic_cast<Label*>(find_by_anchor(*root_, loaded_.slot_anchors[i]));
            if (!label || label->import_binding_instance_id() != slots_[i].instance_id) {
                ++identity_changes_;
                if (label) slots_[i] = {label, label->import_binding_instance_id()};
            }
            if (!label || label->text() != slot_text(i, frame)) ++text_mismatches_;
        }
    }

  private:
    struct Slot {
        Label* label = nullptr;
        std::uint64_t instance_id = 0;
    };

    void materialize(const DesignIR& ir) {
        NativeMaterializeOptions options;
        std::vector<ImportDiagnostic> diagnostics;
        options.diagnostics_out = &diagnostics;
        auto next = build_native_view_tree(ir, ir.asset_manifest, options);
        if (!next) throw std::runtime_error("DesignIR materialization returned null root");
        next->set_bounds({0, 0, loaded_.width, loaded_.height});
        next->layout_children();
        root_ = std::move(next);
    }

    LoadedIr loaded_;
    bool rebuild_each_step_ = false;
    std::unique_ptr<View> root_;
    std::vector<Slot> slots_;
    std::uint64_t identity_changes_ = 0;
    std::uint64_t text_mismatches_ = 0;
};

std::unique_ptr<BenchmarkFixture> make_fixture(const Config& config) {
    if (!config.ir_path.empty())
        return std::make_unique<IrBoundFixture>(config.ir_path,
                                                config.lane == "baked-native-rebuild");
    const auto& lane = config.lane;
    if (lane == "live") return std::make_unique<LiveFixture>();
    if (lane == "baked-native") return std::make_unique<BakedNativeFixture>();
    if (lane == "baked-native-rebuild")
        return std::make_unique<BakedNativeRebuildFixture>();
    if (lane == "baked-cpp") return std::make_unique<BakedCppFixture>();
    return nullptr;
}

std::uint64_t render_frame(View& root) {
    root.layout_children();
    pulp::canvas::RecordingCanvas canvas;
    root.paint_all(canvas);
    return static_cast<std::uint64_t>(canvas.command_count());
}

PhaseMetrics run_phase(BenchmarkFixture& fixture,
                       int duration_ms,
                       std::optional<int> frames,
                       int target_fps,
                       bool interactive) {
    PhaseMetrics metrics;
    metrics.duration_ms = frames ? 0 : duration_ms;
    if (frames ? *frames <= 0 : duration_ms <= 0) return metrics;

    const auto interval = std::chrono::duration<double>(1.0 / std::max(1, target_fps));
    const auto interval_duration = std::chrono::duration_cast<Clock::duration>(interval);
    const auto phase_start = Clock::now();
    const auto cpu_start = std::clock();
    auto next_frame = phase_start;
    const auto bridge_calls_start = fixture.bridge_call_count();
    const auto layout_passes_start = View::layout_pass_count();
    const auto invalidation_start = View::damage_request_count();
    const auto unmarked_start = View::unmarked_paint_mutation_count();
    const auto constructed_start = View::constructed_count();
    const auto destroyed_start = View::destroyed_count();
    const auto identity_start = fixture.slot_identity_change_count();
    const auto mismatch_start = fixture.slot_text_mismatch_count();
    std::vector<double> frame_ms;
    std::vector<double> frame_cpu_ms;
    std::vector<double> update_ms;
    std::vector<double> rss_bytes;

    int frame = 0;
    do {
        const auto frame_start = Clock::now();
        const auto frame_cpu_start = std::clock();
        if (interactive) {
            const auto update_start = Clock::now();
            fixture.step(frame);
            update_ms.push_back(elapsed_ms(update_start));
        }
        metrics.paint_commands_last = render_frame(fixture.root());
        metrics.paint_commands_total += metrics.paint_commands_last;
        ++metrics.paint_calls;
        const auto frame_cpu_end = std::clock();
        const auto frame_end = Clock::now();
        frame_ms.push_back(elapsed_ms(frame_start, frame_end));
        frame_cpu_ms.push_back(cpu_clock_delta_ms(frame_cpu_start, frame_cpu_end));
        const auto rss = current_rss_bytes();
        rss_bytes.push_back(static_cast<double>(rss));
        metrics.rss_peak_bytes = std::max(metrics.rss_peak_bytes, rss);
        ++frame;

        if (frames) continue;
        next_frame += interval_duration;
        const auto now = Clock::now();
        if (next_frame > now) std::this_thread::sleep_until(next_frame);
    } while (frames ? frame < *frames
                    : elapsed_ms(phase_start) < static_cast<double>(duration_ms));

    metrics.samples = static_cast<int>(frame_ms.size());
    if (!frame_ms.empty()) {
        metrics.frame_ms_median = percentile(frame_ms, 50.0);
        metrics.frame_ms_p99 = percentile(frame_ms, 99.0);
        metrics.frame_ms_max = *std::max_element(frame_ms.begin(), frame_ms.end());
    }
    if (!frame_cpu_ms.empty()) {
        metrics.cpu_frame_ms_median = percentile(frame_cpu_ms, 50.0);
        metrics.cpu_frame_ms_p99 = percentile(frame_cpu_ms, 99.0);
    }
    if (!update_ms.empty()) {
        metrics.update_ms_median = percentile(update_ms, 50.0);
        metrics.update_ms_p99 = percentile(update_ms, 99.0);
        metrics.update_ms_max = *std::max_element(update_ms.begin(), update_ms.end());
    }
    if (!rss_bytes.empty()) {
        metrics.rss_median_bytes = static_cast<std::uint64_t>(std::llround(percentile(rss_bytes, 50.0)));
        metrics.rss_p99_bytes = static_cast<std::uint64_t>(std::llround(percentile(rss_bytes, 99.0)));
    }
    const auto cpu_end = std::clock();
    metrics.cpu_ms = cpu_clock_delta_ms(cpu_start, cpu_end);
    metrics.bridge_calls = fixture.bridge_call_count() - bridge_calls_start;
    metrics.layout_passes = View::layout_pass_count() - layout_passes_start;
    metrics.invalidation_requests = View::damage_request_count() - invalidation_start;
    metrics.unmarked_paint_mutations = View::unmarked_paint_mutation_count() - unmarked_start;
    metrics.views_constructed = View::constructed_count() - constructed_start;
    metrics.views_destroyed = View::destroyed_count() - destroyed_start;
    metrics.slot_identity_changes = fixture.slot_identity_change_count() - identity_start;
    metrics.slot_text_mismatches = fixture.slot_text_mismatch_count() - mismatch_start;
    metrics.js_evaluations = fixture.js_evaluation_count();
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

void append_phase_json(std::ostringstream& out,
                       std::string_view name,
                       const PhaseMetrics& metrics,
                       bool trailing_comma) {
    out << "  \"" << name << "\": {\n";
    out << "    \"duration_ms\": " << metrics.duration_ms << ",\n";
    out << "    \"samples\": " << metrics.samples << ",\n";
    out << "    \"cpu_ms\": " << metrics.cpu_ms << ",\n";
    out << "    \"cpu_frame_ms_median\": " << metrics.cpu_frame_ms_median << ",\n";
    out << "    \"cpu_frame_ms_p99\": " << metrics.cpu_frame_ms_p99 << ",\n";
    out << "    \"frame_ms_median\": " << metrics.frame_ms_median << ",\n";
    out << "    \"frame_ms_p99\": " << metrics.frame_ms_p99 << ",\n";
    out << "    \"frame_ms_max\": " << metrics.frame_ms_max << ",\n";
    out << "    \"update_ms_median\": " << metrics.update_ms_median << ",\n";
    out << "    \"update_ms_p99\": " << metrics.update_ms_p99 << ",\n";
    out << "    \"update_ms_max\": " << metrics.update_ms_max << ",\n";
    out << "    \"rss_median_bytes\": " << metrics.rss_median_bytes << ",\n";
    out << "    \"rss_p99_bytes\": " << metrics.rss_p99_bytes << ",\n";
    out << "    \"rss_peak_bytes\": " << metrics.rss_peak_bytes << ",\n";
    out << "    \"paint_commands_last\": " << metrics.paint_commands_last << ",\n";
    out << "    \"js_evaluations_total\": " << metrics.js_evaluations << ",\n";
    out << "    \"counters\": {\n";
    out << "      \"counter_schema\": \"pulp-design-import-benchmark-counters-v1\",\n";
    out << "      \"bridge_calls\": " << metrics.bridge_calls << ",\n";
    out << "      \"layout_passes\": " << metrics.layout_passes << ",\n";
    out << "      \"paint_calls\": " << metrics.paint_calls << ",\n";
    out << "      \"paint_commands_total\": " << metrics.paint_commands_total << ",\n";
    out << "      \"invalidation_requests\": " << metrics.invalidation_requests << ",\n";
    out << "      \"unmarked_paint_mutations\": " << metrics.unmarked_paint_mutations << ",\n";
    out << "      \"views_constructed\": " << metrics.views_constructed << ",\n";
    out << "      \"views_destroyed\": " << metrics.views_destroyed << ",\n";
    out << "      \"slot_identity_changes\": " << metrics.slot_identity_changes << ",\n";
    out << "      \"slot_text_mismatches\": " << metrics.slot_text_mismatches << "\n";
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
};

std::string make_json(const Config& config,
                      const std::optional<IrProvenance>& ir,
                      const StartupMetrics& startup,
                      const PhaseMetrics& idle,
                      const PhaseMetrics& interactive,
                      const TeardownMetrics& teardown) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(4);
    out << "{\n";
    out << "  \"schema\": \"pulp-design-import-benchmark-v1\",\n";
    out << "  \"lane\": \"" << json_escape(config.lane) << "\",\n";
    out << "  \"fixture\": \"" << (ir ? json_escape(ir->file_name) : std::string(kFixtureName))
        << "\",\n";
    if (ir) {
        out << "  \"ir\": {\n";
        out << "    \"sha256\": \"" << ir->sha256 << "\",\n";
        out << "    \"text_slots\": " << ir->slots << "\n";
        out << "  },\n";
    }
    out << "  \"pacing\": \"" << (config.frames ? "fixed-frames" : "wall-clock") << "\",\n";
    if (config.frames) out << "  \"frames_per_phase\": " << *config.frames << ",\n";
    out << "  \"host\": \"" << json_escape(host_name()) << "\",\n";
    out << "  \"platform\": \"" << json_escape(platform_name()) << "\",\n";
    out << "  \"target_fps\": " << config.target_fps << ",\n";
    out << "  \"startup\": {\n";
    out << "    \"build_ms\": " << startup.build_ms << ",\n";
    out << "    \"first_frame_ms\": " << startup.first_frame_ms << ",\n";
    out << "    \"first_frame_render_ms\": " << startup.first_frame_render_ms << ",\n";
    out << "    \"first_frame_paint_commands\": " << startup.first_frame_paint_commands << ",\n";
    out << "    \"rss_after_first_frame_bytes\": " << startup.rss_after_first_frame_bytes << "\n";
    out << "  },\n";
    out << "  \"interaction_model\": \"value drag plus overflow-panel scroll offset; live lane "
           "mutates through JS bridge, baked-native materializes once and uses direct native "
           "setters, baked-native-rebuild rematerializes the same IR per interactive step, "
           "baked-cpp mutates generated-style native widgets directly\",\n";
    append_phase_json(out, "idle", idle, true);
    append_phase_json(out, "interactive", interactive, true);
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
           "[--idle-ms=N] [--interactive-ms=N] [--target-fps=N] [--output=PATH] "
           "[--ir=DESIGN_IR_JSON] [--frames=N] [--plant-leak]\n"
           "  --ir measures an importer-produced DesignIR (baked-native and\n"
           "     baked-native-rebuild only) with anchored text nodes as keyed slots.\n"
           "  --frames runs exactly N frames per phase without pacing, so every\n"
           "     counter in the receipt is reproducible.\n"
           "  --plant-leak leaks one view past teardown; the run must then fail.\n";
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
        } else if (auto value = value_after("--ir=")) {
            config.ir_path = *value;
        } else if (arg == "--ir" && i + 1 < argc) {
            config.ir_path = argv[++i];
        } else if (auto value = value_after("--frames=")) {
            int frames = 0;
            if (!parse_int(*value, frames) || frames < 0) return std::nullopt;
            config.frames = frames;
        } else if (arg == "--plant-leak") {
            config.plant_leak = true;
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
    // The live lane runs a scripted UI and baked-cpp is compiled code; neither
    // can be built from a DesignIR at run time, so neither can claim to have
    // measured one.
    if (!config.ir_path.empty() && config.lane != "baked-native" &&
        config.lane != "baked-native-rebuild") {
        std::cerr << "--ir applies to baked-native and baked-native-rebuild only; the "
                  << config.lane << " lane cannot be built from a DesignIR\n";
        return std::nullopt;
    }
    config.idle_ms = std::max(0, config.idle_ms);
    config.interactive_ms = std::max(0, config.interactive_ms);
    config.target_fps = std::max(1, config.target_fps);
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

    const auto first_render_start = Clock::now();
    const auto first_commands = render_frame(fixture->root());
    const auto first_render_end = Clock::now();

    StartupMetrics startup;
    startup.build_ms = elapsed_ms(build_start, build_end);
    startup.first_frame_render_ms = elapsed_ms(first_render_start, first_render_end);
    startup.first_frame_ms = elapsed_ms(process_start, first_render_end);
    startup.first_frame_paint_commands = first_commands;
    startup.rss_after_first_frame_bytes = current_rss_bytes();

    const auto idle =
        run_phase(*fixture, config->idle_ms, config->frames, config->target_fps, false);
    const auto interactive =
        run_phase(*fixture, config->interactive_ms, config->frames, config->target_fps, true);

    std::optional<IrProvenance> ir;
    if (auto* bound = dynamic_cast<IrBoundFixture*>(fixture.get()))
        ir = IrProvenance{bound->loaded().file_name, bound->loaded().sha256,
                          bound->loaded().slot_anchors.size()};

    // Terminal readback: destroy the fixture, then count the views it left.
    TeardownMetrics teardown;
    teardown.planted_leak = config->plant_leak;
    // The planted subtree outlives the fixture but is freed after the
    // readback, so the negative control is a live view, not a real leak.
    std::unique_ptr<View> planted;
    if (config->plant_leak && fixture->root().child_count() > 0)
        planted = fixture->root().remove_child(fixture->root().child_at(0));
    fixture.reset();
    teardown.views_constructed = View::constructed_count() - views_constructed_start;
    teardown.views_destroyed = View::destroyed_count() - views_destroyed_start;
    teardown.live_views_after_teardown =
        teardown.views_constructed - std::min(teardown.views_constructed, teardown.views_destroyed);
    planted.reset();
    const auto json = make_json(*config, ir, startup, idle, interactive, teardown);

    if (!config->output_path.empty()) {
        if (!write_file(config->output_path, json)) {
            std::cerr << "failed to write " << config->output_path << "\n";
            return 1;
        }
    } else {
        std::cout << json;
    }
    if (teardown.live_views_after_teardown != 0) {
        std::cerr << "teardown left " << teardown.live_views_after_teardown
                  << " live views\n";
        return 3;
    }
    return 0;
}
