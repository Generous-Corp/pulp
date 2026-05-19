// UI Preview — validates the full rendering pipeline with animations
// JS → WidgetBridge → View tree → layout → paint → CoreGraphics/GPU → screen
// Hover over knobs to see glow, click toggle to see animated thumb slide

#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/inspector.hpp>
#include <pulp/inspect/inspector_overlay.hpp>
#include <pulp/inspect/inspector_window.hpp>
#include <pulp/runtime/system.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/window_host.hpp>
#include <pulp/state/store.hpp>
#include <pulp/canvas/bundled_fonts.hpp>
#include <pulp/canvas/text_shaper.hpp>
#include <pulp/view/widgets.hpp>
#include <algorithm>
#include <cmath>
#include <functional>
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
    bool label_audit_enabled = false;
    std::string label_audit_path;

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
        } else if (starts_with(argv[i], "--label-audit")) {
            // pulp #2163 — programmatic label-fit audit. After layout,
            // walk the view tree and for every Label compute the
            // expected glyph extent (real ascent + descent from
            // SkFontMetrics) and compare against the Yoga-assigned
            // box height. Emits one JSON object per Label to stdout.
            // Exits non-zero if any label has glyphs that would clip
            // (glyph_top < 0 or glyph_bottom > box_height).
            //
            // Spec:
            //   --label-audit             prints to stdout
            //   --label-audit=path.json   writes to path.json
            const char* eq = std::strchr(argv[i], '=');
            label_audit_enabled = true;
            if (eq && eq[1] != '\0') label_audit_path = eq + 1;
        } else if (std::strcmp(argv[i], "--font") == 0 && i + 1 < argc) {
            // pulp #2163 — `--font "Family Name=/path/to/font.ttf"` registers
            // a TTF/OTF before the JS bridge starts, so imported designs
            // that reference a host-uninstalled family render with the
            // requested face instead of falling back to the system default
            // (which often lacks Unicode arrows/dashes → tofu boxes).
            // Repeatable: pass --font once per family/variant.
            std::string spec = argv[++i];
            auto eq = spec.find('=');
            if (eq != std::string::npos && eq > 0 && eq + 1 < spec.size()) {
                std::string family = spec.substr(0, eq);
                std::string path = spec.substr(eq + 1);
                if (!pulp::canvas::register_font_file(path, family)) {
                    std::cerr << "[ui-preview] --font: failed to register '"
                              << family << "' from " << path << "\n";
                } else {
                    std::cerr << "[ui-preview] --font: registered '"
                              << family << "' from " << path << "\n";
                }
            } else {
                std::cerr << "[ui-preview] --font expects FAMILY=PATH (got: "
                          << spec << ")\n";
            }
        } else if (starts_with(argv[i], "--font-probe=")) {
            // pulp #2163 — programmatic verification that a font is
            // resolvable AND has a given glyph. Spec is `FAMILY:HEX[,HEX...]`
            // where HEX is a Unicode codepoint (with or without 0x prefix).
            // Example:
            //   --font-probe="IBM Plex Mono:2192,2191"
            // Prints one JSON line per (family, codepoint) and exits with
            // status 0 iff every probe was OK (family resolved AND glyph
            // present). Used by import-design validation, not human eyes.
#ifndef PULP_HAS_SKIA
            std::cerr << "[ui-preview] --font-probe requires Skia "
                         "(built without PULP_HAS_SKIA)\n";
            return 2;
#else
            std::string spec = argv[i] + 13;
            auto colon = spec.find(':');
            if (colon == std::string::npos || colon == 0 || colon + 1 == spec.size()) {
                std::cerr << "[ui-preview] --font-probe expects FAMILY:HEX[,HEX...] (got: "
                          << spec << ")\n";
                return 2;
            }
            std::string family = spec.substr(0, colon);
            std::string cps_str = spec.substr(colon + 1);
            // Honor any --font / --font-dir flags that already ran before
            // this in the argv loop, and any auto-fonts walk later won't
            // matter because we exit here. So the contract is: pass
            // --font / --font-dir BEFORE --font-probe.
            bool all_ok = true;
            size_t p = 0;
            while (p < cps_str.size()) {
                size_t comma = cps_str.find(',', p);
                std::string token = cps_str.substr(p, (comma == std::string::npos ? cps_str.size() : comma) - p);
                p = (comma == std::string::npos) ? cps_str.size() : comma + 1;
                // Strip 0x / U+ prefix if present
                if (token.rfind("0x", 0) == 0 || token.rfind("0X", 0) == 0) token = token.substr(2);
                else if (token.rfind("U+", 0) == 0 || token.rfind("u+", 0) == 0) token = token.substr(2);
                std::uint32_t cp = 0;
                try { cp = static_cast<std::uint32_t>(std::stoul(token, nullptr, 16)); }
                catch (...) {
                    std::cerr << "[ui-preview] --font-probe: bad codepoint '" << token << "'\n";
                    all_ok = false; continue;
                }
                auto pr = pulp::canvas::probe_font_glyph(family, 400, 0, cp);
                std::cout << "{\"family\":\"" << family << "\","
                          << "\"codepoint\":\"U+" << std::hex << std::uppercase
                          << cp << std::dec << std::nouppercase << "\","
                          << "\"family_resolved\":" << (pr.family_resolved ? "true" : "false") << ","
                          << "\"resolved_family\":\"" << pr.resolved_family << "\","
                          << "\"glyph_present\":" << (pr.glyph_present ? "true" : "false") << ","
                          << "\"ok\":" << (pr.family_resolved && pr.glyph_present ? "true" : "false")
                          << "}\n";
                if (!pr.family_resolved || !pr.glyph_present) all_ok = false;
            }
            return all_ok ? 0 : 1;
#endif // PULP_HAS_SKIA
        } else if (starts_with(argv[i], "--font-dir=")) {
            // pulp #2163 — `--font-dir=/path/to/fonts` walks a directory and
            // registers every .ttf / .otf under it. The font family name
            // is parsed from the file's name table (via Skia) rather than
            // from the filename, so `IBMPlexMono-Regular.ttf` resolves
            // under its declared family "IBM Plex Mono" without any
            // mapping work from the caller.
            std::filesystem::path dir{argv[i] + 11};
            std::error_code ec;
            if (!std::filesystem::is_directory(dir, ec)) {
                std::cerr << "[ui-preview] --font-dir: not a directory: "
                          << dir << "\n";
            } else {
                int n = 0;
                for (auto& entry : std::filesystem::recursive_directory_iterator(dir, ec)) {
                    if (!entry.is_regular_file()) continue;
                    auto ext = entry.path().extension().string();
                    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (ext != ".ttf" && ext != ".otf") continue;
                    // Empty family → register_font_file reads the family
                    // out of the OpenType `name` table.
                    if (pulp::canvas::register_font_file(entry.path().string(), "")) ++n;
                }
                std::cerr << "[ui-preview] --font-dir: registered " << n
                          << " font(s) from " << dir << "\n";
            }
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

    // pulp #2163 — co-located-fonts convention. When the imported script
    // lives in a directory, recursively register every .ttf / .otf in
    // that directory and its descendants. Lets a developer drop their
    // JSX + a folder of TTFs side by side (e.g.
    // `~/Desktop/Chainer/ChainerInstrument.jsx` + `~/Desktop/Chainer/IBM_Plex_Mono/...`)
    // and get the requested fonts loaded automatically without any
    // explicit `--font` flag. Family names are read from the OpenType
    // `name` table so common naming variants (`IBMPlexMono-Regular.ttf`
    // resolving to family "IBM Plex Mono") just work.
    //
    // Set PULP_PREVIEW_NO_AUTO_FONTS=1 to opt out.
    if (!script_path.empty() && !std::getenv("PULP_PREVIEW_NO_AUTO_FONTS")) {
        std::error_code ec;
        std::filesystem::path scriptp{script_path};
        std::filesystem::path parent = scriptp.parent_path();
        if (parent.empty()) parent = std::filesystem::current_path(ec);
        if (!ec && std::filesystem::is_directory(parent, ec)) {
            int n = 0;
            for (auto& entry : std::filesystem::recursive_directory_iterator(parent, ec)) {
                if (!entry.is_regular_file()) continue;
                auto ext = entry.path().extension().string();
                for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (ext != ".ttf" && ext != ".otf") continue;
                if (pulp::canvas::register_font_file(entry.path().string(), "")) ++n;
            }
            if (n > 0) {
                std::cerr << "[ui-preview] auto-fonts: registered " << n
                          << " font(s) from " << parent
                          << " (set PULP_PREVIEW_NO_AUTO_FONTS=1 to disable)\n";
            }
        }
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
    // Demo defaults applied ONLY when no --script. Imported scripts
    // (pulp import-design --from jsx output, etc.) manage their own
    // layout — bleeding `padding: 16; gap: 12` into a script's root
    // pushes content inward and offsets every absolute-positioned child.
    // Per user UX feedback 2026-05-17: "no default views end up loaded
    // when a person is simply trying to import their own stuff."
    if (script_path.empty()) {
        root.flex().padding = 16;
        root.flex().gap = 12;
    }

    // Set up scripting engine
    ScriptEngine engine;
    WidgetBridge bridge(engine, root, store);

    // Install runtime-import handlers so imported React/JSX bundles
    // (pulp import-design --from jsx output, etc.) can register +
    // drain useEffect / requestAnimationFrame / setTimeout callbacks.
    // Matches pulp-screenshot's setup — pulp #1899. Without this the
    // bundle's React mount never finishes and the window stays empty.
    bridge.install_runtime_import_handlers();

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
        // After the script load, drain React commit microtasks +
        // useEffect callbacks + initial requestAnimationFrame ticks
        // before the window's first paint. Without this the React tree
        // mounts asynchronously and the first frame shows an empty
        // window. Pumping settle rounds gives React commit + every
        // queued effect a chance to land. Mirrors pulp-screenshot's
        // post-load settle pump (tools/screenshot/pulp_screenshot.cpp).
        bridge.load_script("if (typeof __pulpRuntimeSettle__ === 'function') __pulpRuntimeSettle__(64);");
        std::cout << "Loaded script: " << script_path << "\n";

        // ── Phase 1 instrumentation (pulp jsx-instrument-import / interactivity
        // diagnosis 2026-05-17) — dump React-DOM root delegate state after
        // settle. Per Codex/RepoPrompt: most-likely break is React-DOM never
        // attaching its delegated listeners on document.body (= __root__).
        // Empty result here means the issue is pre-dispatch (Phase 7); non-
        // empty means the bridge between native dispatch and DOM event
        // bubbling is the gap.
        try {
            std::ostringstream js;
            js << "(function(){"
               << "  try {"
               << "    var keys = ['click','mousedown','mouseup','pointerdown',"
               << "                'pointermove','pointerup','wheel','keydown'];"
               << "    var report = {};"
               << "    var targets = {"
               << "      '__root__ (document.body)': (typeof __eventListeners__ !== 'undefined') ? __eventListeners__['__root__'] : null,"
               << "      window_listeners: (typeof window !== 'undefined' && window._listeners) ? window._listeners : null,"
               << "      document_present: typeof document !== 'undefined',"
               << "      document_addEL: typeof document !== 'undefined' && typeof document.addEventListener === 'function',"
               << "      document_dispatch: typeof document !== 'undefined' && typeof document.dispatchEvent === 'function',"
               << "      body_present: typeof document !== 'undefined' && !!document.body,"
               << "      body_id: typeof document !== 'undefined' && document.body ? document.body._id : null,"
               << "    };"
               << "    for (var k in targets) {"
               << "      var v = targets[k];"
               << "      if (v && typeof v === 'object') {"
               << "        var counts = {};"
               << "        for (var i = 0; i < keys.length; i++) {"
               << "          var lst = v[keys[i]];"
               << "          if (lst && lst.length) counts[keys[i]] = lst.length;"
               << "        }"
               << "        report[k] = counts;"
               << "      } else {"
               << "        report[k] = v;"
               << "      }"
               << "    }"
               << "    globalThis.__pulpDiagReport__ = JSON.stringify(report);"
               << "  } catch (e) { globalThis.__pulpDiagReport__ = 'diag error: ' + (e && e.message || e); }"
               << "})();void 0";
            bridge.load_script(js.str());
            bridge.load_script("globalThis.__pulpDiagReport__ || 'no report'");
            // Pull the result back. Hacky — re-evaluate then capture via
            // engine.evaluate to get a string value (load_script discards).
            auto result = engine.evaluate("globalThis.__pulpDiagReport__ || 'no report'");
            std::cout << "[diag] post-settle event-listener state:\n[diag] "
                      << result.getWithDefault(std::string("(empty)")) << "\n";
            std::cout.flush();

            // (Probe 2 removed: had a JS syntax error that crashed the
            // run after settle. Replaced by C++-side native-event
            // instrumentation in window_host_mac.mm + widget_bridge.cpp,
            // gated on PULP_DEBUG_POINTER=1.)

            // Programmatic click probe (PULP_JSX_CLICKPROBE=1). Fires a
            // simulate_drag on the root view at a knob coordinate after
            // settle, dumps React state snapshots before + after, so we
            // can see definitively whether native clicks propagate
            // through to React handlers without depending on the user
            // clicking.
            // Definitive before/after PNG probe (PULP_JSX_PIXEL_PROBE=1).
            // Renders, fires simulate_drag, renders again, compares byte
            // sizes. Same size = no change, different = React state changed.
            if (const char* p2 = std::getenv("PULP_JSX_PIXEL_PROBE"); p2 && *p2) {
                root.set_bounds({0, 0, static_cast<float>(render_w), static_cast<float>(render_h)});
                root.layout_children();
                auto png0 = pulp::view::render_to_png(root, render_w, render_h, 1.0f, pulp::view::ScreenshotBackend::skia);
                std::cout << "[pixel-probe] before drag: png " << png0.size() << " bytes\n";
                std::cout.flush();

                // Try clicking at multiple known-knob coordinates from earlier diagnostic
                // hit-test data: (629,169), (645,162), (647,156), (661,144), (112,61) — all
                // had has_pointer=yes. Use (112, 60) which is the OSC freq knob area.
                root.simulate_drag({112, 60}, {112, 20}, 8);
                bridge.load_script("if (typeof __pulpRuntimeSettle__ === 'function') __pulpRuntimeSettle__(8);");
                root.layout_children();
                auto png1 = pulp::view::render_to_png(root, render_w, render_h, 1.0f, pulp::view::ScreenshotBackend::skia);
                std::cout << "[pixel-probe] after drag:  png " << png1.size() << " bytes  "
                          << "(delta=" << (static_cast<long>(png1.size()) - static_cast<long>(png0.size())) << ")\n";
                std::cout.flush();

                // Also try a click on a button area at the bottom of Chainer
                root.simulate_click({120, 480});  // bottom-bar "save preset" button
                bridge.load_script("if (typeof __pulpRuntimeSettle__ === 'function') __pulpRuntimeSettle__(8);");
                root.layout_children();
                auto png2 = pulp::view::render_to_png(root, render_w, render_h, 1.0f, pulp::view::ScreenshotBackend::skia);
                std::cout << "[pixel-probe] after click: png " << png2.size() << " bytes  "
                          << "(delta=" << (static_cast<long>(png2.size()) - static_cast<long>(png0.size())) << ")\n";
                std::cout.flush();
            }

            // (Periodic stats dump removed — required std::thread / std::this_thread
            // which weren't in scope. Replaced by an explicit JS-side log inside
            // the __dispatch__ bypass using __spectrLog when available, so each
            // bypass fires a line to stderr directly.)

            // Surface shim log + error
            try {
                auto shimLog = engine.evaluate("globalThis.__pulpShimLog__ || '(no shim log)'").getWithDefault(std::string(""));
                std::cout << "--- shim log ---\n" << shimLog;
                auto shimErr = engine.evaluate("globalThis.__pulpShimError__ || ''").getWithDefault(std::string(""));
                if (!shimErr.empty()) std::cout << "[shim-error] " << shimErr << "\n";
                std::cout.flush();
            } catch (...) {}

            // Circle diagnostic — confirms host-config's lowercase circle
            // case fired + d-synth ran.
            try {
                auto circleStats = engine.evaluate("JSON.stringify(globalThis.__pulpCircleStats__ || {total:0, withR:0, samples:[]})").getWithDefault(std::string(""));
                std::cout << "[circle-stats] " << circleStats << "\n";
                std::cout.flush();
            } catch (...) {}

            // Dump the addEventListener / removeEventListener log + actual
            // window._listeners state. The asymmetry between these (15
            // mousemove adds vs 0 in _listeners) is the smoking gun for
            // the slider/XY non-response.
            try {
                auto elLog = engine.evaluate(
                    "(function(){"
                    "  var log = globalThis.__pulpAddELLog__ || [];"
                    "  var addCounts = {}, remCounts = {};"
                    "  for (var i = 0; i < log.length; i++) {"
                    "    var e = log[i];"
                    "    if (e.op === 'add') addCounts[e.type] = (addCounts[e.type] || 0) + 1;"
                    "    if (e.op === 'remove') remCounts[e.type] = (remCounts[e.type] || 0) + 1;"
                    "  }"
                    "  var actual = {};"
                    "  if (typeof window !== 'undefined' && window._listeners) {"
                    "    for (var t in window._listeners) {"
                    "      var lst = window._listeners[t];"
                    "      if (lst && lst.length) actual[t] = lst.length;"
                    "    }"
                    "  }"
                    "  var sampleStacks = log.filter(function(e){return e.op === 'add' && e.type === 'mousemove'}).slice(0, 2).map(function(e){return e.stack});"
                    "  return JSON.stringify({log_add: addCounts, log_remove: remCounts, actual_listeners: actual, mousemove_addstacks: sampleStacks});"
                    "})()").getWithDefault(std::string(""));
                std::cout << "[addEL-audit] " << elLog << "\n";
                std::cout.flush();
            } catch (...) {}

            // Patch __dispatch__ to count __global__ events. Helps verify
            // the drag fan-out path firing into window listeners.
            try {
                engine.evaluate(
                    "(function(){"
                    "  if (typeof __dispatch__ !== 'function') return;"
                    "  if (__dispatch__.__pulpInstrumented__) return;"
                    "  var orig = __dispatch__;"
                    "  globalThis.__pulpGlobalCounts__ = {};"
                    "  globalThis.__pulpGlobalLastListenerCounts__ = {};"
                    "  __dispatch__ = function(id, eventName){"
                    "    if (id === '__global__') {"
                    "      globalThis.__pulpGlobalCounts__[eventName] = (globalThis.__pulpGlobalCounts__[eventName] || 0) + 1;"
                    "      var wl = (typeof window !== 'undefined' && window._listeners && window._listeners[eventName]) ? window._listeners[eventName].length : 0;"
                    "      globalThis.__pulpGlobalLastListenerCounts__[eventName] = wl;"
                    "    }"
                    "    return orig.apply(this, arguments);"
                    "  };"
                    "  __dispatch__.__pulpInstrumented__ = true;"
                    "})();void 0");
            } catch (...) {}

            // DOM structure probe — find where React actually mounted.
            if (const char* p3 = std::getenv("PULP_JSX_DOM_PROBE"); p3 && *p3) {
                auto info = engine.evaluate(
                    "(function(){"
                    "  var report = {};"
                    "  report.bodyExists = !!(document && document.body);"
                    "  report.bodyId = document && document.body && document.body._id;"
                    "  report.bodyChildren = document && document.body && document.body._children ? document.body._children.length : -1;"
                    "  report.documentElement = document && document.documentElement ? document.documentElement._id : null;"
                    "  report.nativeElementCount = (typeof __nativeElements__ !== 'undefined') ? Object.keys(__nativeElements__).length : -1;"
                    "  report.firstTenIds = (typeof __nativeElements__ !== 'undefined') ? Object.keys(__nativeElements__).slice(0,10) : [];"
                    "  // walk EVERY native element looking for SVG primitives"
                    "  var svgKinds = {svg:0, path:0, circle:0, line:0, rect:0, button:0, div:0, span:0, input:0};"
                    "  if (typeof __nativeElements__ !== 'undefined') {"
                    "    for (var k in __nativeElements__) {"
                    "      var el = __nativeElements__[k];"
                    "      var t = el && el.tagName ? el.tagName.toLowerCase() : '?';"
                    "      if (svgKinds[t] !== undefined) svgKinds[t]++;"
                    "    }"
                    "  }"
                    "  report.tagCounts = svgKinds;"
                    "  // Check if any element has a parentElement chain leading back to body"
                    "  var leafSample = null, leafChain = [];"
                    "  if (typeof __nativeElements__ !== 'undefined') {"
                    "    for (var k in __nativeElements__) {"
                    "      var el = __nativeElements__[k];"
                    "      if (el && el.tagName && el.tagName.toLowerCase() === 'path') { leafSample = el; break; }"
                    "    }"
                    "    if (leafSample) {"
                    "      var p = leafSample;"
                    "      while (p && leafChain.length < 12) { leafChain.push(p._id + '(' + p.tagName + ')'); p = p._parentElement; }"
                    "    }"
                    "  }"
                    "  report.leafChain = leafChain;"
                    "  report.leafChainConnectsToBody = leafChain.length > 0 && leafChain[leafChain.length-1].indexOf('__root__') >= 0;"
                    "  return JSON.stringify(report);"
                    "})()"
                ).getWithDefault(std::string("(empty)"));
                std::cout << "[dom-probe] " << info << "\n";
                std::cout.flush();
            }

            if (const char* probe = std::getenv("PULP_JSX_CLICKPROBE"); probe && *probe) {
                root.set_bounds({0, 0, static_cast<float>(render_w), static_cast<float>(render_h)});
                root.layout_children();

                // Walk the React tree and find the deepest path/circle/svg element
                // we can hit. Dump its rect + id.
                std::ostringstream find_js;
                find_js << "(function(){"
                        << "  var visit = [];"
                        << "  function walk(el, depth) {"
                        << "    if (!el || depth > 32) return;"
                        << "    var tag = (el.tagName || '').toLowerCase();"
                        << "    if (tag === 'path' || tag === 'circle' || tag === 'line' || tag === 'button') {"
                        << "      var rect = (typeof getLayoutRect === 'function') ? getLayoutRect(el._id) : null;"
                        << "      visit.push({ id: el._id, tag: tag, rect: rect });"
                        << "    }"
                        << "    if (el._children) for (var i = 0; i < el._children.length; i++) walk(el._children[i], depth + 1);"
                        << "  }"
                        << "  if (document && document.body) walk(document.body, 0);"
                        << "  return JSON.stringify(visit.slice(0, 10));"
                        << "})()";
                auto find = engine.evaluate(find_js.str()).getWithDefault(std::string("(empty)"));
                std::cout << "[probe] first 10 leaf hits: " << find << "\n";

                // Fire simulate_drag at a hard-coded knob coordinate
                // (OSC freq knob, roughly y=70 in Chainer based on
                // earlier diagnostic — fits 1280x800 viewport).
                std::cout << "[probe] firing simulate_drag at (150, 220) → (150, 180)\n";
                root.simulate_drag({150, 220}, {150, 180}, 6);

                // Pump a few settle rounds for React state updates to commit.
                bridge.load_script("if (typeof __pulpRuntimeSettle__ === 'function') __pulpRuntimeSettle__(8);");

                // Try to read Chainer's React state. Chainer stores params via
                // useState inside ChainerInstrument(). We can't reach React's
                // internal state, but if React re-rendered then the DOM has
                // new attribute values. Try reading the freq display value.
                auto dom = engine.evaluate(
                    "(function(){"
                    "  var found = null;"
                    "  function walk(el, d){"
                    "    if (!el || d > 24) return;"
                    "    var txt = el._textContent || '';"
                    "    if (/\\bhz\\b/.test(txt) && txt.length < 20) { found = txt; return; }"
                    "    if (el._children) for (var i = 0; i < el._children.length && !found; i++) walk(el._children[i], d+1);"
                    "  }"
                    "  if (document && document.body) walk(document.body, 0);"
                    "  return found || '(no hz display found)';"
                    "})()"
                ).getWithDefault(std::string("(empty)"));
                std::cout << "[probe] hz display text after drag: " << dom << "\n";
                std::cout.flush();
            }
        } catch (const std::exception& e) {
            std::cout << "[diag] failed: " << e.what() << "\n";
        }
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

    // pulp #2163 — run label-fit audit BEFORE screenshot_only short-
    // circuits so headless audit runs work without launching a window.
    // Layout is already done; the audit is a tree walk + arithmetic.
    auto run_label_audit = [&](int width, int height) -> int {
        root.set_bounds({0, 0, static_cast<float>(width), static_cast<float>(height)});
        root.layout_children();
        std::ostream* audit_out = &std::cout;
        std::ofstream audit_file;
        if (!label_audit_path.empty()) {
            audit_file.open(label_audit_path);
            if (audit_file.is_open()) audit_out = &audit_file;
        }
        int total = 0, clipping = 0;
        std::function<void(pulp::view::View*)> walk;
        walk = [&](pulp::view::View* v) {
            if (!v) return;
            if (auto* l = dynamic_cast<pulp::view::Label*>(v)) {
                const auto& b = l->bounds();
                float font_size = l->font_size();
                std::string family = l->font_family();
                std::string family_for_metrics = family.empty() ? std::string("Inter") : family;
                auto& shaper = pulp::canvas::global_text_shaper();
                auto prepared = shaper.prepare(
                    l->text().empty() ? std::string(" ") : l->text(),
                    family_for_metrics, font_size);
                float ascent = prepared.ascent();
                float descent = prepared.descent();
                float line_height = prepared.line_height();
                bool real_metrics = prepared.metrics_are_real();
                float baseline_y_top = ascent > 0 ? ascent : font_size * 0.85f;
                float glyph_top = baseline_y_top - ascent;
                float glyph_bottom = baseline_y_top + descent;
                bool fits = (glyph_top >= -0.5f) && (glyph_bottom <= b.height + 0.5f)
                            && (b.height >= ascent + descent - 0.5f);
                total++;
                if (!fits) clipping++;
                *audit_out << "{"
                    << "\"text\":\"" << l->text() << "\","
                    << "\"family\":\"" << family << "\","
                    << "\"font_size\":" << font_size << ","
                    << "\"box\":{\"x\":" << b.x << ",\"y\":" << b.y
                                << ",\"width\":" << b.width
                                << ",\"height\":" << b.height << "},"
                    << "\"metrics\":{\"ascent\":" << ascent
                                    << ",\"descent\":" << descent
                                    << ",\"line_height\":" << line_height
                                    << ",\"real\":" << (real_metrics ? "true" : "false") << "},"
                    << "\"baseline_y_top_align\":" << baseline_y_top << ","
                    << "\"glyph_top\":" << glyph_top << ","
                    << "\"glyph_bottom\":" << glyph_bottom << ","
                    << "\"fits\":" << (fits ? "true" : "false")
                    << "}\n";
            }
            for (size_t i = 0; i < v->child_count(); ++i) walk(v->child_at(i));
        };
        walk(&root);
        std::cerr << "[label-audit] " << total << " labels checked, "
                  << clipping << " would clip\n";
        return clipping;
    };

    if (label_audit_enabled && screenshot_only) {
        run_label_audit(render_w, render_h);
        // continue to screenshot anyway
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

    std::cout << "Hover over knobs to see glow animation\n";
    std::cout << "Click the toggle to see animated thumb slide\n";
    std::cout << "Hover over fader to see thumb scale\n";

    WindowOptions opts;
    // Title from the imported script's filename when --script is given,
    // otherwise the demo default. Imported JSX/TSX files like
    // ChainerInstrument.jsx → "ChainerInstrument" (drop directory +
    // extension). Per user UX feedback 2026-05-17.
    if (!script_path.empty()) {
        std::filesystem::path p(script_path);
        opts.title = p.stem().string();
        if (opts.title.empty()) opts.title = "Pulp Imported";
    } else {
        opts.title = "Pulp Animation Preview";
    }
    // Wire the parsed --size through to the live window. Pre-fix this
    // was hardcoded 360x480, ignoring --size entirely — imported JSX
    // (pulp import-design --from jsx) ended up materialising at the
    // requested viewport for headless paint but the live window stayed
    // at the demo size. Codex consult 2026-05-17 flagged this as the
    // first preview bug to fix.
    opts.width = render_w;
    opts.height = render_h;
    // pulp jsx-instrument-import — use GPU rendering by default for the
    // live preview path. Imported JSX bundles ship SVG (knob rings,
    // waveform paths, chain-viz arrows) that benefit from Skia GPU;
    // CPU rasterization is fine for static screenshots but the live
    // path should match production plugin hosting which is GPU. Per
    // user 2026-05-17.
    opts.use_gpu = true;

    // For imported JSX bundles, auto-size the window height to the
    // measured content after the React settle pass so we don't ship
    // 200px of dead background below the bottombar. Width stays at
    // the requested --size; only height shrinks (never grows past the
    // requested cap). Pre-resize-measure path: read root_'s laid-out
    // children's bottom edge after `__pulpRuntimeSettle__(64)` has
    // pumped React commit + initial useEffects, then clamp the window
    // height to max(measured + small bottombar inset, 240).
    if (!script_path.empty()) {
        // Pre-size root_ to the requested viewport BEFORE host attach
        // so the imported tree measures against the right width and
        // its flex/wrap behavior is computed correctly. Mirrors the
        // pulp-screenshot pre-bounds setup at
        // tools/screenshot/pulp_screenshot.cpp:190.
        root.set_bounds({0, 0, static_cast<float>(render_w), static_cast<float>(render_h)});
        root.layout_children();

        // Walk root's direct children to find the bottommost paint
        // edge. The imported React tree typically mounts as a single
        // <div id="root"> wrapper containing the user's component.
        float content_bottom = 0.0f;
        for (size_t i = 0; i < root.child_count(); ++i) {
            auto* child = root.child_at(i);
            if (!child) continue;
            const auto b = child->bounds();
            content_bottom = std::max(content_bottom, b.y + b.height);
        }
        std::cout << "[ui-preview] measured content_bottom=" << content_bottom
                  << " child_count=" << root.child_count()
                  << " render_h=" << render_h << "\n";
        std::cout.flush();
        if (content_bottom > 32.0f && content_bottom < static_cast<float>(render_h)) {
            // Tighten the window height to fit content. Floor of 240
            // prevents pathological collapses on tiny fixtures.
            opts.height = std::max(240, static_cast<int>(std::ceil(content_bottom)));
            std::cout << "[ui-preview] auto-sized height to content: "
                      << opts.height << " (measured " << content_bottom
                      << ", requested " << render_h << ")\n";
            std::cout.flush();
        }
    }

    // pulp #2163 — label-fit audit. Walks the laid-out tree, computes
    // expected glyph extent per Label, flags labels whose glyphs would
    // clip given Yoga-assigned box height. JSON output for tooling.
    if (label_audit_enabled) {
        std::ostream* audit_out = &std::cout;
        std::ofstream audit_file;
        if (!label_audit_path.empty()) {
            audit_file.open(label_audit_path);
            if (audit_file.is_open()) audit_out = &audit_file;
        }
        int total = 0;
        int clipping = 0;
        std::function<void(pulp::view::View*)> walk;
        walk = [&](pulp::view::View* v) {
            if (!v) return;
            if (auto* l = dynamic_cast<pulp::view::Label*>(v)) {
                const auto& b = l->bounds();
                float font_size = l->font_size();
                std::string family = l->font_family();
                std::string family_for_metrics = family.empty() ? std::string("Inter") : family;
                auto& shaper = pulp::canvas::global_text_shaper();
                auto prepared = shaper.prepare(
                    l->text().empty() ? std::string(" ") : l->text(),
                    family_for_metrics, font_size);
                float ascent = prepared.ascent();
                float descent = prepared.descent();
                float line_height = prepared.line_height();
                bool real_metrics = prepared.metrics_are_real();
                // Compute baseline_y exactly the way paint() does for
                // the default vertical_align (top). center variants
                // differ but for this audit we report the top-align
                // case — it's the worst case for "first line of new
                // section" clipping which is the symptom we're after.
                float baseline_y_top = ascent > 0 ? ascent : font_size * 0.85f;
                float glyph_top = baseline_y_top - ascent;
                float glyph_bottom = baseline_y_top + descent;
                bool fits = (glyph_top >= -0.5f) && (glyph_bottom <= b.height + 0.5f)
                            && (b.height >= ascent + descent - 0.5f);
                total++;
                if (!fits) clipping++;
                *audit_out << "{"
                    << "\"text\":\"" << l->text() << "\","
                    << "\"family\":\"" << family << "\","
                    << "\"font_size\":" << font_size << ","
                    << "\"box\":{\"x\":" << b.x << ",\"y\":" << b.y
                                << ",\"width\":" << b.width
                                << ",\"height\":" << b.height << "},"
                    << "\"metrics\":{\"ascent\":" << ascent
                                    << ",\"descent\":" << descent
                                    << ",\"line_height\":" << line_height
                                    << ",\"real\":" << (real_metrics ? "true" : "false") << "},"
                    << "\"baseline_y_top_align\":" << baseline_y_top << ","
                    << "\"glyph_top\":" << glyph_top << ","
                    << "\"glyph_bottom\":" << glyph_bottom << ","
                    << "\"fits\":" << (fits ? "true" : "false")
                    << "}\n";
            }
            for (size_t i = 0; i < v->child_count(); ++i) walk(v->child_at(i));
        };
        walk(&root);
        std::cerr << "[label-audit] " << total << " labels checked, "
                  << clipping << " would clip\n";
        if (label_audit_path.empty()) return clipping == 0 ? 0 : 2;
    }

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
