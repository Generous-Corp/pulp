// UI Preview — validates the full rendering pipeline with animations
// JS → WidgetBridge → View tree → layout → paint → CoreGraphics/GPU → screen
// Hover over knobs to see glow, click toggle to see animated thumb slide

#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/inspector.hpp>
#include <pulp/view/motion.hpp>
#include <pulp/inspect/inspector_overlay.hpp>
#include <pulp/inspect/inspector_server.hpp>
#include <pulp/inspect/inspector_window.hpp>
#include <pulp/inspect/domain_handler.hpp>
#include <pulp/inspect/motion_inspector.hpp>
#include <pulp/runtime/system.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/window_host.hpp>
#include <pulp/state/store.hpp>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <filesystem>

#ifdef PULP_BENCHMARK
#include <pulp/render/bench/perf_counters.hpp>
#include <pulp/view/visualization_bridge.hpp>
#include <choc/text/choc_JSON.h>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>
#include <thread>
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#endif

using namespace pulp::view;
using namespace pulp::state;

namespace {

struct AutomationConfig {
    bool enabled = false;
    std::string click_point;
    std::string view_id;
    std::string view_type;
    std::string view_text;
    std::string view_label;
    std::filesystem::path before_path;
    std::filesystem::path after_path;
    int delay_ms = 200;
    int after_delay_ms = 750;
    bool exit_after = true;
};

std::string env_string(const char* name) {
    if (const char* value = std::getenv(name)) return value;
    return {};
}

int env_int(const char* name, int fallback) {
    if (const char* value = std::getenv(name)) {
        try {
            return std::stoi(value);
        } catch (...) {
        }
    }
    return fallback;
}

bool env_flag(const char* name, bool fallback) {
    if (const char* value = std::getenv(name)) {
        std::string text = value;
        if (text == "1" || text == "true" || text == "TRUE" || text == "yes") return true;
        if (text == "0" || text == "false" || text == "FALSE" || text == "no") return false;
    }
    return fallback;
}

AutomationConfig load_automation_config() {
    AutomationConfig config;
    config.view_id = env_string("PULP_AUTOMATION_CLICK_VIEW_ID");
    config.click_point = env_string("PULP_AUTOMATION_CLICK_POINT");
    config.view_type = env_string("PULP_AUTOMATION_CLICK_VIEW_TYPE");
    config.view_text = env_string("PULP_AUTOMATION_CLICK_VIEW_TEXT");
    config.view_label = env_string("PULP_AUTOMATION_CLICK_VIEW_LABEL");
    auto before = env_string("PULP_AUTOMATION_BEFORE_OUT");
    auto after = env_string("PULP_AUTOMATION_AFTER_OUT");
    if (!before.empty()) config.before_path = before;
    if (!after.empty()) config.after_path = after;
    config.delay_ms = env_int("PULP_AUTOMATION_DELAY_MS", 200);
    config.after_delay_ms = env_int("PULP_AUTOMATION_AFTER_DELAY_MS", 750);
    config.exit_after = env_flag("PULP_AUTOMATION_EXIT_AFTER", true);
    config.enabled =
        !config.click_point.empty() || !config.view_id.empty() || !config.view_type.empty() || !config.view_text.empty() ||
        !config.view_label.empty() || !config.before_path.empty() || !config.after_path.empty();
    return config;
}

bool parse_point(const std::string& text, pulp::view::Point& point) {
    auto comma = text.find(",");
    if (comma == std::string::npos) return false;
    try {
        point.x = std::stof(text.substr(0, comma));
        point.y = std::stof(text.substr(comma + 1));
        return true;
    } catch (...) {
        return false;
    }
}

bool write_binary_file(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    if (path.empty()) return true;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

void advance_widget_animations(View* view, float dt) {
    if (!view) return;
    if (auto* knob = dynamic_cast<Knob*>(view)) knob->advance_animations(dt);
    else if (auto* toggle = dynamic_cast<Toggle*>(view)) toggle->advance_animations(dt);
    else if (auto* fader = dynamic_cast<Fader*>(view)) fader->advance_animations(dt);
    else if (auto* scroll = dynamic_cast<ScrollView*>(view)) scroll->advance_animations(dt);
    else if (auto* tooltip = dynamic_cast<Tooltip*>(view)) tooltip->advance_animations(dt);

    for (size_t i = 0; i < view->child_count(); ++i) {
        advance_widget_animations(view->child_at(i), dt);
    }
}

bool selector_matches(const View& view, const AutomationConfig& config) {
    if (!config.view_id.empty() && view.id() != config.view_id) return false;
    if (!config.view_type.empty() && ViewInspector::type_name(view) != config.view_type) return false;

    if (!config.view_text.empty()) {
        auto* label = dynamic_cast<const Label*>(&view);
        if (!label || label->text() != config.view_text) return false;
    }

    if (!config.view_label.empty()) {
        bool matched = false;
        if (auto* toggle = dynamic_cast<const Toggle*>(&view)) matched = toggle->label() == config.view_label;
        else if (auto* knob = dynamic_cast<const Knob*>(&view)) matched = knob->label() == config.view_label;
        else if (auto* fader = dynamic_cast<const Fader*>(&view)) matched = fader->label() == config.view_label;
        else if (auto* label = dynamic_cast<const Label*>(&view)) matched = label->text() == config.view_label;
        if (!matched) return false;
    }

    return true;
}

View* find_first_matching_view(View& root, const AutomationConfig& config) {
    if (selector_matches(root, config)) return &root;
    for (size_t i = 0; i < root.child_count(); ++i) {
        if (auto* found = find_first_matching_view(*root.child_at(i), config)) return found;
    }
    return nullptr;
}

pulp::view::Point center_in_root(const View& root, const View& target) {
    pulp::view::Point center{
        target.bounds().width * 0.5f,
        target.bounds().height * 0.5f,
    };

    auto* current = &target;
    while (current && current != &root) {
        center.x += current->bounds().x;
        center.y += current->bounds().y;
        current = current->parent();
    }
    return center;
}

bool write_view_tree(const std::string& path, View& root, int width, int height) {
    if (path.empty()) return true;
    root.set_bounds({0, 0, static_cast<float>(width), static_cast<float>(height)});
    root.layout_children();

    auto out_path = std::filesystem::path(path);
    if (out_path.has_parent_path()) std::filesystem::create_directories(out_path.parent_path());
    std::ofstream out(out_path);
    if (!out.is_open()) return false;
    out << ViewInspector::to_json(root) << "\n";
    return true;
}

#ifdef PULP_BENCHMARK

// ── Zero-copy benchmark mode (Slice 0 of #516) ──────────────────────────────
//
// Headless frame-paced loop that drives VisualizationBridge with a
// deterministic sine sweep. No WindowHost is created. Emits a JSON
// blob matching tools/scripts/bench_diff.py's documented schema on the
// `--output=<path>` file.
//
// NOTE: the JSON key naming is deliberately mixed:
//   - `audio_to_triplebuffer_copy`  (no _us suffix — legacy consumer)
//   - `triplebuffer_publish_latency` (no _us suffix — legacy consumer)
//   - `gpu_upload_us`, `gpu_readback_us`, `gpu_dispatch_us`,
//     `total_frame_us` (with _us suffix)
// bench_diff.py has already shipped and consumes these exact field
// names — do not "fix" the inconsistency.

struct BenchmarkConfig {
    bool enabled = false;
    int seconds = 10;
    std::string widget = "oscilloscope";
    std::string output_path;
    int target_fps = 60;
};

std::string current_iso8601_utc() {
    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &tt);
#else
    gmtime_r(&tt, &tm_utc);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    return buf;
}

std::string current_short_sha() {
    // Shell out to `git rev-parse --short HEAD`. Best-effort: if we're
    // running from a detached tarball the return will be empty and the
    // JSON will carry "unknown".
    std::FILE* pipe = ::popen("git rev-parse --short HEAD 2>/dev/null", "r");
    if (!pipe) return "unknown";
    char buf[64] = {0};
    std::string out;
    while (std::fgets(buf, sizeof(buf), pipe) != nullptr) {
        out += buf;
    }
    ::pclose(pipe);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) {
        out.pop_back();
    }
    return out.empty() ? std::string("unknown") : out;
}

std::string current_host_short() {
    char buf[256] = {0};
    if (::gethostname(buf, sizeof(buf) - 1) == 0) {
        std::string nodename = buf;
        auto dot = nodename.find('.');
        if (dot != std::string::npos) nodename.resize(dot);
        return nodename;
    }
    return "unknown";
}

std::string current_platform_tag() {
#if defined(__APPLE__)
#if defined(__aarch64__)
    return "darwin-arm64";
#else
    return "darwin-x86_64";
#endif
#elif defined(_WIN32)
#if defined(_M_ARM64)
    return "windows-arm64";
#else
    return "windows-x86_64";
#endif
#elif defined(__linux__)
#if defined(__aarch64__)
    return "linux-arm64";
#else
    return "linux-x86_64";
#endif
#else
    return "unknown";
#endif
}

int run_benchmark(const BenchmarkConfig& cfg) {
    using namespace pulp::view;

    std::cout << "Pulp zero-copy benchmark\n"
              << "  widget:        " << cfg.widget << "\n"
              << "  seconds:       " << cfg.seconds << "\n"
              << "  target FPS:    " << cfg.target_fps << "\n"
              << "  output:        " << cfg.output_path << "\n";

    if (cfg.output_path.empty()) {
        std::cerr << "--benchmark-seconds requires --output=<path>\n";
        return 2;
    }

    // Build the bridge and wire up perf counters.
    VisualizationBridge bridge;
    VisualizationConfig vcfg;
    vcfg.fft_size = 1024;
    vcfg.hop_size = 256;
    vcfg.num_channels = 2;
    vcfg.sample_rate = 44100.0f;
    vcfg.capture_waveform = (cfg.widget == "oscilloscope");
    vcfg.waveform_length = 1024;
    bridge.configure(vcfg);

    pulp::render::bench::PerfCounters counters;
    counters.reset();
    bridge.set_bench_counters(&counters);

    // Deterministic stereo sine sweep input. One audio block per
    // frame; 256 samples/block at 44.1 kHz is ~5.8 ms of audio per
    // rendered frame — close enough to real-time cadence that the
    // per-frame costs mean the right thing.
    constexpr int kBlockSize = 256;
    std::vector<float> ch0(kBlockSize);
    std::vector<float> ch1(kBlockSize);
    const float* channels[2] = {ch0.data(), ch1.data()};

    const int total_frames = cfg.target_fps * cfg.seconds;
    const double frame_budget_us = 1.0e6 / static_cast<double>(cfg.target_fps);

    double phase0 = 0.0;
    double phase1 = 0.0;
    const double two_pi = 6.283185307179586476925286766559;
    const double sr = static_cast<double>(vcfg.sample_rate);

    for (int f = 0; f < total_frames; ++f) {
        const double frame_t0 = pulp::render::bench::now_us();

        // Sine sweep: 100 Hz → 4 kHz over `cfg.seconds`, left/right
        // slightly detuned so the FFT bins are non-trivial.
        const double sweep_progress =
            static_cast<double>(f) / static_cast<double>(std::max(1, total_frames - 1));
        const double freq_l = 100.0 + sweep_progress * 3900.0;
        const double freq_r = freq_l * 1.01;
        const double dp0 = two_pi * freq_l / sr;
        const double dp1 = two_pi * freq_r / sr;
        for (int i = 0; i < kBlockSize; ++i) {
            ch0[i] = static_cast<float>(std::sin(phase0));
            ch1[i] = static_cast<float>(std::sin(phase1));
            phase0 += dp0;
            phase1 += dp1;
            if (phase0 > two_pi) phase0 -= two_pi;
            if (phase1 > two_pi) phase1 -= two_pi;
        }
        bridge.process(channels, /*num_channels=*/2, kBlockSize);

        const double frame_dt = pulp::render::bench::now_us() - frame_t0;
        counters.total_frame_total_us.fetch_add(
            frame_dt, std::memory_order_relaxed);
        counters.sample_count.fetch_add(1.0, std::memory_order_relaxed);

        // Frame-pace to the target FPS. If the work finished under
        // budget, sleep out the remainder so that `total_frame_us`
        // reflects real work, not wall-clock cadence.
        const double remaining_us = frame_budget_us - frame_dt;
        if (remaining_us > 1.0) {
            std::this_thread::sleep_for(
                std::chrono::microseconds(static_cast<int64_t>(remaining_us)));
        }
    }

    const auto snap = counters.snapshot_and_reset();
    const double n = std::max(1.0, snap.sample_count);

    auto per_frame_us = choc::value::createObject("");
    per_frame_us.addMember("audio_to_triplebuffer_copy",
                           snap.audio_copy_total_us / n);
    per_frame_us.addMember("triplebuffer_publish_latency",
                           snap.triplebuffer_publish_total_us / n);
    per_frame_us.addMember("gpu_upload_us",
                           snap.gpu_upload_total_us / n);
    per_frame_us.addMember("gpu_readback_us",
                           snap.gpu_readback_total_us / n);
    per_frame_us.addMember("gpu_dispatch_us",
                           snap.gpu_dispatch_total_us / n);
    per_frame_us.addMember("total_frame_us",
                           snap.total_frame_total_us / n);

    auto per_frame_bytes = choc::value::createObject("");
    per_frame_bytes.addMember("cpu_to_gpu_bytes",
                              snap.cpu_to_gpu_bytes_total / n);
    per_frame_bytes.addMember("gpu_to_cpu_bytes",
                              snap.gpu_to_cpu_bytes_total / n);

    // Memory-bandwidth fraction: memory-moving buckets / (samples * budget).
    // GPU dispatch is compute, not bandwidth — excluded.
    const double memory_us_total = snap.audio_copy_total_us
                                 + snap.triplebuffer_publish_total_us
                                 + snap.gpu_upload_total_us
                                 + snap.gpu_readback_total_us;
    const double budget_total_us = snap.sample_count * frame_budget_us;
    const double memory_bandwidth_fraction =
        budget_total_us > 0.0 ? (memory_us_total / budget_total_us) : 0.0;

    auto root = choc::value::createObject("");
    root.addMember("host", current_host_short());
    root.addMember("date", current_iso8601_utc());
    root.addMember("pulp_commit", current_short_sha());
    root.addMember("platform", current_platform_tag());
    root.addMember("widget", cfg.widget);
    root.addMember("seconds", static_cast<int64_t>(cfg.seconds));
    root.addMember("target_fps", static_cast<int64_t>(cfg.target_fps));
    root.addMember("samples", static_cast<int64_t>(snap.sample_count));
    root.addMember("per_frame_us", per_frame_us);
    root.addMember("per_frame_bytes", per_frame_bytes);
    root.addMember("frame_budget_us", static_cast<int64_t>(frame_budget_us));
    root.addMember("memory_bandwidth_fraction", memory_bandwidth_fraction);

    auto json_str = choc::json::toString(root, /*pretty=*/true);

    auto out_path = std::filesystem::path(cfg.output_path);
    if (out_path.has_parent_path()) {
        std::filesystem::create_directories(out_path.parent_path());
    }
    std::ofstream out(out_path);
    if (!out.is_open()) {
        std::cerr << "Failed to open " << cfg.output_path << " for writing\n";
        return 1;
    }
    out << json_str << "\n";
    out.close();

    std::cout << "Benchmark complete. samples=" << static_cast<int64_t>(snap.sample_count)
              << " memory_bandwidth_fraction="
              << (memory_bandwidth_fraction * 100.0) << "%\n"
              << "JSON written to " << cfg.output_path << "\n";
    return 0;
}

#endif  // PULP_BENCHMARK

}  // namespace

int main(int argc, char* argv[]) {
    bool screenshot_only = false;
    std::string screenshot_path = "/tmp/pulp-animation-preview.png";
    std::string view_tree_path;
    std::string script_path;
    int render_w = 360, render_h = 480;

#ifdef PULP_BENCHMARK
    BenchmarkConfig bench_cfg;
#endif

    auto starts_with = [](const char* s, const char* prefix) -> bool {
        while (*prefix) {
            if (*s++ != *prefix++) return false;
        }
        return true;
    };

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--screenshot") == 0) {
            screenshot_only = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') screenshot_path = argv[++i];
        } else if (std::strcmp(argv[i], "--script") == 0 && i + 1 < argc) {
            script_path = argv[++i];
        } else if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            std::string sz = argv[++i];
            auto x = sz.find('x');
            if (x != std::string::npos) {
                render_w = std::stoi(sz.substr(0, x));
                render_h = std::stoi(sz.substr(x + 1));
            }
        } else if (std::strcmp(argv[i], "--view-tree-out") == 0 && i + 1 < argc) {
            view_tree_path = argv[++i];
        }
#ifdef PULP_BENCHMARK
        else if (starts_with(argv[i], "--benchmark-seconds=")) {
            bench_cfg.enabled = true;
            try { bench_cfg.seconds = std::stoi(argv[i] + 20); } catch (...) {}
        } else if (starts_with(argv[i], "--widget=")) {
            bench_cfg.widget = argv[i] + 9;
        } else if (starts_with(argv[i], "--output=")) {
            bench_cfg.output_path = argv[i] + 9;
        } else if (starts_with(argv[i], "--target-fps=")) {
            try { bench_cfg.target_fps = std::stoi(argv[i] + 13); } catch (...) {}
        }
#endif
    }

#ifdef PULP_BENCHMARK
    if (bench_cfg.enabled) {
        return run_benchmark(bench_cfg);
    }
#endif

    if (view_tree_path.empty()) {
        if (const char* env_path = std::getenv("PULP_VIEW_TREE_OUT")) view_tree_path = env_path;
    }

    const auto automation = load_automation_config();

    // Set up parameters
    StateStore store;
    store.add_parameter({1, "Gain", "dB", {-60.0f, 12.0f, 0.0f}});
    store.add_parameter({2, "Mix", "%", {0.0f, 100.0f, 100.0f}});
    store.add_parameter({3, "Bypass", "", {0.0f, 1.0f, 0.0f}});

    // Create root view with dark theme and animation clock
    FrameClock clock;
    View root;
    root.set_theme(Theme::dark());
    root.set_frame_clock(&clock);
    root.flex().direction = FlexDirection::column;
    root.flex().padding = 16;
    root.flex().gap = 12;

    // Set up scripting engine
    ScriptEngine engine;
    WidgetBridge bridge(engine, root, store);

    // Build UI — from script file or built-in demo
    if (!script_path.empty()) {
        std::ifstream sf(script_path);
        if (!sf.is_open()) {
            std::cerr << "Cannot open script: " << script_path << "\n";
            return 1;
        }
        std::ostringstream ss;
        ss << sf.rdbuf();
        bridge.load_script(ss.str());
        std::cout << "Loaded script: " << script_path << "\n";
    } else {
        auto title = std::make_unique<Label>("Pulp Animation Preview");
        title->set_font_size(16.0f);
        title->flex().preferred_height = 24;
        root.add_child(std::move(title));

        auto knob_row = std::make_unique<View>();
        knob_row->flex().direction = FlexDirection::row;
        knob_row->flex().gap = 16;
        knob_row->flex().preferred_height = 80;

        auto gain = std::make_unique<Knob>();
        gain->set_label("Gain");
        gain->set_value(0.5f);
        gain->flex().preferred_width = 64;
        gain->flex().preferred_height = 64;
        knob_row->add_child(std::move(gain));

        auto mix = std::make_unique<Knob>();
        mix->set_label("Mix");
        mix->set_value(0.8f);
        mix->flex().preferred_width = 64;
        mix->flex().preferred_height = 64;
        knob_row->add_child(std::move(mix));

        root.add_child(std::move(knob_row));

        auto bypass = std::make_unique<Toggle>();
        bypass->set_id("bypass-toggle");
        bypass->set_label("Bypass");
        bypass->flex().preferred_width = 60;
        bypass->flex().preferred_height = 28;
        root.add_child(std::move(bypass));

        auto fader = std::make_unique<Fader>();
        fader->set_label("Volume");
        fader->set_value(0.65f);
        fader->set_orientation(Fader::Orientation::horizontal);
        fader->flex().preferred_height = 32;
        root.add_child(std::move(fader));

        auto scroll = std::make_unique<ScrollView>();
        scroll->flex().flex_grow = 1;
        scroll->set_content_size({0, 600});
        for (int i = 0; i < 20; i++) {
            auto item = std::make_unique<Label>("Preset " + std::to_string(i + 1));
            item->set_bounds({8, static_cast<float>(i * 28 + 4), 300, 24});
            scroll->add_child(std::move(item));
        }
        root.add_child(std::move(scroll));
    }

    std::cout << "UI Preview: " << root.child_count() << " widgets created\n";

    auto emit_view_tree = [&](int width, int height) -> bool {
        if (!write_view_tree(view_tree_path, root, width, height)) {
            std::cerr << "Failed to write view tree to " << view_tree_path << "\n";
            return false;
        }
        if (!view_tree_path.empty()) std::cout << "View tree saved to " << view_tree_path << "\n";
        return true;
    };

    // Set up inspector before screenshot so it renders in headless mode too
    pulp::inspect::InspectorOverlay inspector(root);
    pulp::inspect::install_inspector_hooks(inspector);
    if (pulp::runtime::get_env("PULP_INSPECTOR")) {
        inspector.set_active(true);
    }

    if (screenshot_only) {
        if (!emit_view_tree(render_w, render_h)) return 1;
        bool ok = render_to_file(
            root,
            static_cast<uint32_t>(render_w),
            static_cast<uint32_t>(render_h),
            screenshot_path.c_str());
        std::cout << (ok ? "Screenshot saved to " + screenshot_path + "\n" : "Screenshot failed\n");
        pulp::inspect::g_active_inspector = nullptr;
        return ok ? 0 : 1;
    }

    // ── Motion observability wiring ─────────────────────────────────
    //
    // Bind the motion coordinator to the FrameClock so traces sample
    // each tick. The InspectorServer + MotionInspector pair come up
    // only when PULP_MOTION_SERVER=1 (defaults to off so the example
    // doesn't open a TCP port unexpectedly). Logs always go through
    // the default log sink when PULP_MOTION_LOG=1. A scope guard
    // unbinds the coordinator before the local FrameClock dies, so
    // the singleton's destructor never touches a dangling pointer.
    pulp::view::motion::Coordinator::instance().bind(clock);
    int motion_log_sink_id = 0;
    std::unique_ptr<pulp::inspect::InspectorServer> motion_server;
    std::unique_ptr<pulp::inspect::MotionInspector> motion_inspector;
    std::unique_ptr<pulp::inspect::DomainHandler> motion_dispatch;
    struct MotionGuard {
        int& log_sink_id;
        std::unique_ptr<pulp::inspect::MotionInspector>& inspector_ref;
        ~MotionGuard() {
            inspector_ref.reset();
            auto& c = pulp::view::motion::Coordinator::instance();
            if (log_sink_id) c.remove_sink(log_sink_id);
            c.unbind();
        }
    } motion_guard{motion_log_sink_id, motion_inspector};

    if (pulp::runtime::get_env("PULP_MOTION_LOG")) {
        motion_log_sink_id =
            pulp::view::motion::Coordinator::instance().install_default_log_sink();
        pulp::view::motion::Coordinator::instance().set_tracing_enabled(true);
    }
    if (pulp::runtime::get_env("PULP_MOTION_SERVER")) {
        motion_server = std::make_unique<pulp::inspect::InspectorServer>();
        if (motion_server->start()) {
            motion_inspector = std::make_unique<pulp::inspect::MotionInspector>(
                root, motion_server.get());
            motion_dispatch = std::make_unique<pulp::inspect::DomainHandler>();
            motion_dispatch->set_root_view(&root);
            motion_dispatch->set_motion_inspector(motion_inspector.get());
            auto* dispatch = motion_dispatch.get();
            motion_server->set_request_handler(
                [dispatch](const pulp::inspect::InspectorMessage& req) {
                    return dispatch->handle(req);
                });
            motion_server->advertise_port();
            std::cout << "Motion inspector listening on port "
                      << motion_server->port() << "\n";
            pulp::view::motion::Coordinator::instance().set_tracing_enabled(true);
        } else {
            std::cerr << "Motion inspector server failed to start; "
                         "PULP_MOTION_SERVER ignored.\n";
            motion_server.reset();
        }
    }

    std::cout << "Hover over knobs to see glow animation\n";
    std::cout << "Click the toggle to see animated thumb slide\n";
    std::cout << "Hover over fader to see thumb scale\n";

    WindowOptions opts;
    opts.title = "Pulp Animation Preview";
    opts.width = 360;
    opts.height = 480;

    if (!emit_view_tree(opts.width, opts.height)) return 1;

    auto window = WindowHost::create(root, opts);
    int automation_exit_code = 0;
    window->set_close_callback([] { std::cout << "Window closed\n"; });

#if defined(__APPLE__)
    if (automation.enabled) {
        auto automation_copy = automation;
        auto* window_ptr = window.get();
        auto* root_ptr = &root;
        auto* view_tree_path_ptr = &view_tree_path;
        auto* automation_exit_code_ptr = &automation_exit_code;

        dispatch_after(
            dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(automation_copy.delay_ms) * NSEC_PER_MSEC),
            dispatch_get_main_queue(),
            ^{
                auto fail = [&](const std::string& message) {
                    std::cerr << "Automation failed: " << message << "\n";
                    *automation_exit_code_ptr = 1;
                    if (window_ptr) window_ptr->request_close();
                };

                try {
                    root_ptr->set_bounds({0, 0, opts.width, opts.height});
                    root_ptr->layout_children();
                    window_ptr->repaint();

                    if (!automation_copy.before_path.empty()) {
                        auto before_png = render_to_png(*root_ptr, static_cast<uint32_t>(opts.width), static_cast<uint32_t>(opts.height));
                        if (before_png.empty() || !write_binary_file(automation_copy.before_path, before_png)) {
                            fail("failed to capture before image");
                            return;
                        }
                    }

                    pulp::view::Point click_point{};
                    if (!automation_copy.click_point.empty()) {
                        if (!parse_point(automation_copy.click_point, click_point)) {
                            fail("invalid automation click point");
                            return;
                        }
                    } else {
                        auto* target = find_first_matching_view(*root_ptr, automation_copy);
                        if (!target) {
                            fail("no matching view for automation selector");
                            return;
                        }
                        click_point = center_in_root(*root_ptr, *target);
                    }
                    root_ptr->simulate_click(click_point);
                    advance_widget_animations(root_ptr, std::max(automation_copy.after_delay_ms, 0) / 1000.0f);
                    if (auto* toggle = dynamic_cast<Toggle*>(find_first_matching_view(*root_ptr, automation_copy))) {
                        std::cout << "Automation toggle state=" << (toggle->is_on() ? "on" : "off")
                                  << " thumb=" << toggle->thumb_position() << "\n";
                    }
                    root_ptr->layout_children();
                    window_ptr->repaint();

                    dispatch_after(
                        dispatch_time(DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC),
                        dispatch_get_main_queue(),
                        ^{
                            try {
                                if (!view_tree_path_ptr->empty() &&
                                    !write_view_tree(*view_tree_path_ptr, *root_ptr, static_cast<int>(opts.width), static_cast<int>(opts.height))) {
                                    fail("failed to write view tree");
                                    return;
                                }
                                if (!automation_copy.after_path.empty()) {
                                    auto after_png = render_to_png(*root_ptr, static_cast<uint32_t>(opts.width), static_cast<uint32_t>(opts.height));
                                    if (after_png.empty() || !write_binary_file(automation_copy.after_path, after_png)) {
                                        fail("failed to capture after image");
                                        return;
                                    }
                                }
                            } catch (const std::exception& e) {
                                fail(std::string("automation finalize error: ") + e.what());
                                return;
                            } catch (...) {
                                fail("automation finalize error");
                                return;
                            }

                            if (automation_copy.exit_after) window_ptr->request_close();
                        });
                } catch (const std::exception& e) {
                    fail(std::string("automation error: ") + e.what());
                } catch (...) {
                    fail("automation error");
                }
            });
    }
#else
    if (automation.enabled) {
        std::cerr << "Automation mode is only implemented on macOS for ui-preview today.\n";
        return 1;
    }
#endif

    // Inspector: open a separate floating window when Cmd+I is pressed
    std::unique_ptr<WindowHost> inspector_window;
    auto inspector_view = std::make_unique<pulp::inspect::InspectorWindow>(root);
    auto* inspector_view_ptr = inspector_view.get();
    View* inspector_selected = nullptr;

    auto open_inspector = [&]() {
        if (inspector_window) return;
        WindowOptions iopts;
        iopts.title = "Inspector";
        iopts.width = 340;
        iopts.height = static_cast<float>(opts.height);
        iopts.resizable = true;
        iopts.use_gpu = false;
        inspector_window = WindowHost::create(*inspector_view_ptr, iopts);
        if (inspector_window) {
            inspector_window->set_close_callback([&] { inspector_window.reset(); });
            inspector_window->show();
            inspector_window->position_beside(window.get());
            inspector_view_ptr->refresh();
        }
    };

    View::set_inspector_key_hook([&](const KeyEvent& e) -> bool {
        if (e.is_down && e.key == KeyCode::i && e.isMainModifier()) {
            if (!inspector_window) open_inspector();
            else { inspector_window.reset(); inspector_selected = nullptr; }
            return true;
        }
        return false;
    });

    View::set_inspector_mouse_hook([&](const MouseEvent& e) -> bool {
        if (!inspector_window) return false;
        if (e.is_down && e.isMainModifier()) {
            auto* hit = root.hit_test(e.position);
            if (hit) {
                inspector_selected = hit;
                inspector_view_ptr->select_view(hit);
                if (inspector_window) inspector_window->repaint();
                if (window) window->repaint();
            }
            return true;
        }
        return false;
    });

    inspector_view_ptr->on_view_selected = [&](View* view) {
        inspector_selected = view;
        if (window) window->repaint();
    };

    View::set_inspector_paint_hook([&](pulp::canvas::Canvas& canvas) {
        // Always paint the highlight overlay when a view is selected and the
        // inspector is open. No consumed-flag — this avoids flicker during
        // fast repaints (e.g., dragging a knob).
        if (!inspector_selected || !inspector_window) return;
        float x = 0, y = 0;
        const View* cur = inspector_selected;
        while (cur && cur != &root) { x += cur->bounds().x; y += cur->bounds().y; cur = cur->parent(); }
        float w = inspector_selected->bounds().width, h = inspector_selected->bounds().height;
        canvas.set_fill_color(pulp::canvas::Color::rgba(0.25f, 0.5f, 1.0f, 0.15f));
        canvas.fill_rect(x, y, w, h);
        canvas.set_stroke_color(pulp::canvas::Color::rgba(0.25f, 0.5f, 1.0f, 0.8f));
        canvas.set_line_width(2.0f);
        canvas.stroke_rect(x, y, w, h);
    });

    window->set_idle_callback([&] {
        if (inspector_window) {
            // Only refresh the active tab's data (refresh() already does this).
            // The NSTimer fires at 30 Hz regardless of window focus.
            inspector_view_ptr->refresh();
            inspector_window->repaint();
            window->repaint();
        }
    });

    if (pulp::runtime::get_env("PULP_INSPECTOR")) open_inspector();

    std::cout << "Opening window... (Cmd+I for inspector)\n";
    window->run_event_loop();
    return automation_exit_code;
}
