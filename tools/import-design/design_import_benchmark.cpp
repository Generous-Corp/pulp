#include <pulp/canvas/canvas.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/design_import.hpp>
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
    std::uint64_t rss_median_bytes = 0;
    std::uint64_t rss_p99_bytes = 0;
    std::uint64_t rss_peak_bytes = 0;
    std::uint64_t paint_commands_last = 0;
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
    node.type = "audio_widget";
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

DesignIR build_baked_ir() {
    DesignIR ir;
    ir.source = DesignSource::jsx;
    ir.source_adapter = "phase5-benchmark-fixture";
    ir.capture_method = "hand-authored-live-equivalent";

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
    panel.children.push_back(audio_node("output", AudioWidgetType::meter, "Output", 0.55f, 380, 72, 42, 160));

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
    return ir;
}

View* find_by_id(View& root, std::string_view id) {
    if (root.id() == id) return &root;
    for (std::size_t i = 0; i < root.child_count(); ++i) {
        if (auto* found = find_by_id(*root.child_at(i), id)) return found;
    }
    return nullptr;
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

std::unique_ptr<BenchmarkFixture> make_fixture(const std::string& lane) {
    if (lane == "live") return std::make_unique<LiveFixture>();
    if (lane == "baked-native") return std::make_unique<BakedNativeFixture>();
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
                       int target_fps,
                       bool interactive) {
    PhaseMetrics metrics;
    metrics.duration_ms = duration_ms;
    if (duration_ms <= 0) return metrics;

    const auto interval = std::chrono::duration<double>(1.0 / std::max(1, target_fps));
    const auto interval_duration = std::chrono::duration_cast<Clock::duration>(interval);
    const auto phase_start = Clock::now();
    const auto cpu_start = std::clock();
    auto next_frame = phase_start;
    std::vector<double> frame_ms;
    std::vector<double> frame_cpu_ms;
    std::vector<double> rss_bytes;

    int frame = 0;
    while (elapsed_ms(phase_start) < static_cast<double>(duration_ms)) {
        const auto frame_start = Clock::now();
        const auto frame_cpu_start = std::clock();
        if (interactive) fixture.step(frame);
        metrics.paint_commands_last = render_frame(fixture.root());
        const auto frame_cpu_end = std::clock();
        const auto frame_end = Clock::now();
        frame_ms.push_back(elapsed_ms(frame_start, frame_end));
        frame_cpu_ms.push_back(cpu_clock_delta_ms(frame_cpu_start, frame_cpu_end));
        const auto rss = current_rss_bytes();
        rss_bytes.push_back(static_cast<double>(rss));
        metrics.rss_peak_bytes = std::max(metrics.rss_peak_bytes, rss);
        ++frame;

        next_frame += interval_duration;
        const auto now = Clock::now();
        if (next_frame > now) std::this_thread::sleep_until(next_frame);
    }

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
    if (!rss_bytes.empty()) {
        metrics.rss_median_bytes = static_cast<std::uint64_t>(std::llround(percentile(rss_bytes, 50.0)));
        metrics.rss_p99_bytes = static_cast<std::uint64_t>(std::llround(percentile(rss_bytes, 99.0)));
    }
    const auto cpu_end = std::clock();
    metrics.cpu_ms = cpu_clock_delta_ms(cpu_start, cpu_end);
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
    out << "    \"rss_median_bytes\": " << metrics.rss_median_bytes << ",\n";
    out << "    \"rss_p99_bytes\": " << metrics.rss_p99_bytes << ",\n";
    out << "    \"rss_peak_bytes\": " << metrics.rss_peak_bytes << ",\n";
    out << "    \"paint_commands_last\": " << metrics.paint_commands_last << ",\n";
    out << "    \"js_evaluations_total\": " << metrics.js_evaluations << "\n";
    out << "  }" << (trailing_comma ? "," : "") << "\n";
}

std::string make_json(const Config& config,
                      const StartupMetrics& startup,
                      const PhaseMetrics& idle,
                      const PhaseMetrics& interactive) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(4);
    out << "{\n";
    out << "  \"schema\": \"pulp-design-import-benchmark-v1\",\n";
    out << "  \"lane\": \"" << json_escape(config.lane) << "\",\n";
    out << "  \"fixture\": \"" << kFixtureName << "\",\n";
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
    out << "  \"interaction_model\": \"value drag plus overflow-panel scroll offset; live lane mutates through JS bridge, baked-native lane mutates native widgets directly\",\n";
    append_phase_json(out, "idle", idle, true);
    append_phase_json(out, "interactive", interactive, false);
    out << "}\n";
    return out.str();
}

void print_usage(std::ostream& out) {
    out << "usage: pulp-design-import-bench [--lane=live|baked-native] "
           "[--idle-ms=N] [--interactive-ms=N] [--target-fps=N] [--output=PATH]\n";
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
        } else if (auto value = value_after("--output=")) {
            config.output_path = *value;
        } else if (arg == "--output" && i + 1 < argc) {
            config.output_path = argv[++i];
        } else {
            return std::nullopt;
        }
    }
    if (config.lane != "live" && config.lane != "baked-native") return std::nullopt;
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

    const auto build_start = Clock::now();
    std::unique_ptr<BenchmarkFixture> fixture;
    try {
        fixture = make_fixture(config->lane);
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

    const auto idle = run_phase(*fixture, config->idle_ms, config->target_fps, false);
    const auto interactive = run_phase(*fixture, config->interactive_ms, config->target_fps, true);
    const auto json = make_json(*config, startup, idle, interactive);

    if (!config->output_path.empty()) {
        if (!write_file(config->output_path, json)) {
            std::cerr << "failed to write " << config->output_path << "\n";
            return 1;
        }
    } else {
        std::cout << json;
    }
    return 0;
}
