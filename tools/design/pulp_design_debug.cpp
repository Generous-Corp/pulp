// pulp-design-debug — headless before/after/diff runner for design chat prompts
// Reuses the live design-tool JS prompt builder and response applier so debug
// artifacts stay aligned with the actual product path.

#include <pulp/view/view.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/screenshot_compare.hpp>
#include <pulp/state/store.hpp>
#include <pulp/runtime/system.hpp>

#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

using namespace pulp::view;
using namespace pulp::state;
namespace fs = std::filesystem;

namespace {

enum class CaptureMode {
    headless_skia,
    headless_coregraphics,
    live_gpu
};

struct Options {
    fs::path script_path;
    fs::path design_tool_bin;
    fs::path output_dir = fs::path("build") / "design-debug";
    std::string prompt;
    std::string target = "all";
    std::string provider = "claude";
    std::string model = "claude-sonnet-4-6";
    std::string reasoning_effort;
    std::string ai_cli;
    fs::path response_file;
    uint32_t width = 1100;
    uint32_t height = 700;
    float scale = 2.0f;
    CaptureMode capture_mode = CaptureMode::headless_skia;
    ScreenshotBackend capture_backend = ScreenshotBackend::skia;
    uint64_t delay_ms = 350;
    uint64_t after_delay_ms = 350;
    bool debug_json = true;
};

std::string read_file(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool write_text_file(const fs::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;
    out << content;
    return out.good();
}

bool write_binary_file(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

std::vector<uint8_t> read_binary_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool append_text_file(const fs::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out.is_open()) return false;
    out << content;
    return out.good();
}

std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += ' ';
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

std::string slugify(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    bool prev_dash = false;
    for (char c : text) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc)) {
            out.push_back(static_cast<char>(std::tolower(uc)));
            prev_dash = false;
        } else if (!prev_dash) {
            out.push_back('-');
            prev_dash = true;
        }
    }
    while (!out.empty() && out.front() == '-') out.erase(out.begin());
    while (!out.empty() && out.back() == '-') out.pop_back();
    if (out.empty()) out = "prompt";
    if (out.size() > 48) out.resize(48);
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out;
}

// Ordered design-tool concern modules. The example UI was split out of a
// single design-tool.js (P8-NEW); the modules share one global scope and only
// form a valid program when loaded in this order — their concatenation is
// byte-equivalent to the historical single file. Keep in sync with
// kDesignToolModules in examples/design-tool/main.cpp.
constexpr const char* kDesignToolEntry = "design-tool-core.js";
constexpr const char* kDesignToolModules[] = {
    "design-tool-core.js",
    "design-tool-toolbar.js",
    "design-tool-palette.js",
    "design-tool-popup.js",
    "design-tool-preview.js",
    "design-tool-chat.js",
    "design-tool-export.js",
};

fs::path find_repo_root() {
    auto dir = fs::current_path();
    while (!dir.empty()) {
        if (fs::exists(dir / "CMakeLists.txt")
            && fs::exists(dir / "examples" / "design-tool" / kDesignToolEntry)) {
            return dir;
        }
        auto parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return {};
}

fs::path find_default_script(const fs::path& repo_root) {
    auto path = repo_root / "examples" / "design-tool" / kDesignToolEntry;
    if (fs::exists(path)) return path;
    return {};
}

fs::path find_default_design_tool_bin(const fs::path& repo_root) {
    auto path = repo_root / "build" / "examples" / "design-tool" / "pulp-design-tool";
    if (fs::exists(path)) return path;
    return {};
}

void load_design_tool(const fs::path& script_path, WidgetBridge& bridge) {
    auto js_dir = script_path.parent_path();
    auto oklch_path = js_dir / "oklch.js";
    if (fs::exists(oklch_path)) {
        bridge.load_script(read_file(oklch_path));
    }
    // When the script is the design-tool entry module, load the full ordered
    // concern-module set from its directory (the modules only form a valid
    // program in sequence). For any other --script path, load it directly.
    if (script_path.filename() == kDesignToolEntry) {
        for (const char* module : kDesignToolModules) {
            bridge.load_script(read_file(js_dir / module));
        }
    } else {
        bridge.load_script(read_file(script_path));
    }
}

void print_usage() {
    std::cerr << "Usage: pulp-design-debug --prompt <text> [options]\n";
    std::cerr << "  --target <id|all>            Scope prompt to a widget like k1, slider1, t1\n";
    std::cerr << "  --script <file.js>           Design tool entry module (default: examples/design-tool/design-tool-core.js)\n";
    std::cerr << "  --output-dir <dir>           Artifact directory (default: build/design-debug)\n";
    std::cerr << "  --response-file <file.txt>   Use a saved model response instead of calling AI\n";
    std::cerr << "  --provider <name>            Metadata provider name (default: claude)\n";
    std::cerr << "  --model <id>                 Metadata/model id (default: claude-sonnet-4-6)\n";
    std::cerr << "  --reasoning-effort <level>   Metadata effort (low|medium|high|xhigh)\n";
    std::cerr << "  --ai-cli <template>          Override AI CLI template; supports {prompt_file} {model} {provider} {reasoning_effort}\n";
    std::cerr << "  --width <px>                 Render width (default: 1100)\n";
    std::cerr << "  --height <px>                Render height (default: 700)\n";
    std::cerr << "  --scale <factor>             Render scale (default: 2.0)\n";
    std::cerr << "  --capture-backend <name>     Screenshot backend: skia, coregraphics, or live-gpu (default: skia)\n";
    std::cerr << "  --design-tool-bin <path>     Live design-tool binary for --capture-backend live-gpu\n";
    std::cerr << "  --delay-ms <ms>              Delay before baseline capture in live-gpu mode (default: 350)\n";
    std::cerr << "  --after-delay-ms <ms>        Delay before post-apply capture in live-gpu mode (default: 350)\n";
}

std::string shell_quote(std::string_view text) {
    std::string out = "'";
    for (char c : text) {
        if (c == '\'') out += "'\"'\"'";
        else out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

std::string run_command_capture(const std::string& command, int& exit_code) {
    std::array<char, 4096> buffer{};
    std::string output;
#if defined(_WIN32)
    FILE* pipe = _popen(command.c_str(), "r");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (!pipe) {
        exit_code = -1;
        return {};
    }
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }
#if defined(_WIN32)
    auto status = _pclose(pipe);
    exit_code = status;
#else
    auto status = pclose(pipe);
    if (WIFEXITED(status)) exit_code = WEXITSTATUS(status);
    else exit_code = status;
#endif
    return output;
}

std::string now_stamp() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
    return buf;
}

const char* capture_mode_flag_name(CaptureMode mode, ScreenshotBackend backend) {
    if (mode == CaptureMode::live_gpu) return "live-gpu";
    switch (backend) {
        case ScreenshotBackend::coregraphics: return "coregraphics";
        case ScreenshotBackend::skia: return "skia";
        default: return "default";
    }
}

const char* capture_mode_report_name(CaptureMode mode, ScreenshotBackend backend) {
    if (mode == CaptureMode::live_gpu) return "skia-live-gpu";
    switch (backend) {
        case ScreenshotBackend::coregraphics: return "coregraphics-headless";
        case ScreenshotBackend::skia: return "skia-headless";
        default: return "unknown";
    }
}

bool capture_mode_supports_widget_sksl(CaptureMode mode, ScreenshotBackend backend) {
    return mode == CaptureMode::live_gpu || backend == ScreenshotBackend::skia;
}

struct ParsedTargetBounds {
    bool valid = false;
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

ParsedTargetBounds parse_target_bounds(std::string_view json) {
    static const std::regex re(
        R"("targetBounds"\s*:\s*\{\s*"x"\s*:\s*([-0-9.]+)\s*,\s*"y"\s*:\s*([-0-9.]+)\s*,\s*"width"\s*:\s*([-0-9.]+)\s*,\s*"height"\s*:\s*([-0-9.]+)\s*\})",
        std::regex::ECMAScript);
    std::match_results<std::string_view::const_iterator> match;
    ParsedTargetBounds bounds;
    if (!std::regex_search(json.begin(), json.end(), match, re) || match.size() != 5) {
        return bounds;
    }
    bounds.valid = true;
    bounds.x = std::stof(std::string(match[1].first, match[1].second));
    bounds.y = std::stof(std::string(match[2].first, match[2].second));
    bounds.width = std::stof(std::string(match[3].first, match[3].second));
    bounds.height = std::stof(std::string(match[4].first, match[4].second));
    return bounds;
}

struct TargetArtifacts {
    bool valid = false;
    bool diff_written = false;
    CompareResult compare;
    std::string source = "none";
    fs::path before_path;
    fs::path after_path;
    fs::path diff_path;
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t padding = 0;
};

} // namespace

int main(int argc, char* argv[]) {
    Options opts;
    auto repo_root = find_repo_root();
    if (!repo_root.empty()) {
        opts.script_path = find_default_script(repo_root);
        opts.design_tool_bin = find_default_design_tool_bin(repo_root);
    }

    try {
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--prompt" && i + 1 < argc) opts.prompt = argv[++i];
            else if (arg == "--target" && i + 1 < argc) opts.target = argv[++i];
            else if (arg == "--script" && i + 1 < argc) opts.script_path = argv[++i];
            else if (arg == "--output-dir" && i + 1 < argc) opts.output_dir = argv[++i];
            else if (arg == "--response-file" && i + 1 < argc) opts.response_file = argv[++i];
            else if (arg == "--provider" && i + 1 < argc) opts.provider = argv[++i];
            else if (arg == "--model" && i + 1 < argc) opts.model = argv[++i];
            else if (arg == "--reasoning-effort" && i + 1 < argc) opts.reasoning_effort = argv[++i];
            else if (arg == "--ai-cli" && i + 1 < argc) opts.ai_cli = argv[++i];
            else if (arg == "--width" && i + 1 < argc) opts.width = static_cast<uint32_t>(std::stoul(argv[++i]));
            else if (arg == "--height" && i + 1 < argc) opts.height = static_cast<uint32_t>(std::stoul(argv[++i]));
            else if (arg == "--scale" && i + 1 < argc) opts.scale = std::stof(argv[++i]);
            else if (arg == "--design-tool-bin" && i + 1 < argc) opts.design_tool_bin = argv[++i];
            else if (arg == "--delay-ms" && i + 1 < argc) opts.delay_ms = static_cast<uint64_t>(std::stoull(argv[++i]));
            else if (arg == "--after-delay-ms" && i + 1 < argc) opts.after_delay_ms = static_cast<uint64_t>(std::stoull(argv[++i]));
            else if (arg == "--capture-backend" && i + 1 < argc) {
                auto value = std::string(argv[++i]);
                if (value == "coregraphics") {
                    opts.capture_mode = CaptureMode::headless_coregraphics;
                    opts.capture_backend = ScreenshotBackend::coregraphics;
                } else if (value == "skia") {
                    opts.capture_mode = CaptureMode::headless_skia;
                    opts.capture_backend = ScreenshotBackend::skia;
                } else if (value == "live-gpu") {
                    opts.capture_mode = CaptureMode::live_gpu;
                    opts.capture_backend = ScreenshotBackend::skia;
                }
                else {
                    std::cerr << "Unknown capture backend: " << value << "\n";
                    return 1;
                }
            }
            else if (arg == "--help" || arg == "-h") {
                print_usage();
                return 0;
            } else {
                std::cerr << "Unknown option: " << arg << "\n";
                print_usage();
                return 1;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Invalid option value: " << e.what() << "\n";
        print_usage();
        return 1;
    } catch (...) {
        std::cerr << "Invalid option value\n";
        print_usage();
        return 1;
    }

    if (opts.prompt.empty()) {
        std::cerr << "Error: --prompt is required\n";
        print_usage();
        return 1;
    }
    if (opts.script_path.empty() || !fs::exists(opts.script_path)) {
        std::cerr << "Error: design tool script not found\n";
        return 1;
    }
    if (opts.capture_mode == CaptureMode::live_gpu &&
        (opts.design_tool_bin.empty() || !fs::exists(opts.design_tool_bin))) {
        std::cerr << "Error: live GPU capture requires a built design tool binary. "
                     "Expected " << opts.design_tool_bin << "\n";
        return 1;
    }
    if (!opts.response_file.empty() && !fs::exists(opts.response_file)) {
        std::cerr << "Error: response file not found: " << opts.response_file << "\n";
        return 1;
    }
    fs::create_directories(opts.output_dir);

    auto stamp = now_stamp();
    auto stem = stamp + "-" + slugify(opts.target == "all" ? "all" : opts.target) + "-" +
                slugify(opts.model) + "-" + slugify(opts.prompt);
    auto before_path = opts.output_dir / (stem + "-before.png");
    auto after_path = opts.output_dir / (stem + "-after.png");
    auto diff_path = opts.output_dir / (stem + "-diff.png");
    auto prompt_path = opts.output_dir / (stem + "-prompt.txt");
    auto response_path = opts.output_dir / (stem + "-response.txt");
    auto debug_state_path = opts.output_dir / (stem + "-debug-state.json");
    auto apply_summary_path = opts.output_dir / (stem + "-apply-summary.txt");
    auto report_path = opts.output_dir / (stem + "-report.json");
    auto latest_report_path = opts.output_dir / "latest-report.json";
    auto latest_run_path = opts.output_dir / "latest-run.json";
    auto runs_path = opts.output_dir / "runs.jsonl";

    std::string prompt_text;
    std::string response_text;
    std::string command_string;
    std::string command_output;
    std::string apply_summary;
    std::string debug_state_json = "{}";
    int command_exit = 0;
    std::vector<uint8_t> before_png;
    std::vector<uint8_t> after_png;
    float target_x = 0.0f, target_y = 0.0f, target_w = 0.0f, target_h = 0.0f;
    bool target_found = false;

    if (opts.capture_mode == CaptureMode::live_gpu) {
        std::ostringstream cmd;
        cmd << shell_quote(opts.design_tool_bin.string())
            << " --script " << shell_quote(opts.script_path.string())
            << " --automation-prompt " << shell_quote(opts.prompt)
            << " --automation-target " << shell_quote(opts.target)
            << " --automation-provider " << shell_quote(opts.provider)
            << " --automation-model " << shell_quote(opts.model)
            << " --automation-delay-ms " << opts.delay_ms
            << " --automation-after-delay-ms " << opts.after_delay_ms
            << " --automation-before " << shell_quote(before_path.string())
            << " --automation-after " << shell_quote(after_path.string())
            << " --automation-prompt-out " << shell_quote(prompt_path.string())
            << " --automation-response-out " << shell_quote(response_path.string())
            << " --automation-debug-state-out " << shell_quote(debug_state_path.string())
            << " --automation-apply-summary-out " << shell_quote(apply_summary_path.string());
        if (!opts.reasoning_effort.empty()) {
            cmd << " --automation-reasoning-effort " << shell_quote(opts.reasoning_effort);
        }
        if (!opts.ai_cli.empty()) {
            cmd << " --automation-ai-cli " << shell_quote(opts.ai_cli);
        }
        if (!opts.response_file.empty()) {
            cmd << " --automation-response-file " << shell_quote(opts.response_file.string());
        }
        command_string = cmd.str();
        command_output = run_command_capture(command_string, command_exit);
        if (command_exit != 0) {
            std::cerr << "Live GPU automation failed with exit code " << command_exit << "\n";
            if (!command_output.empty()) std::cerr << command_output << "\n";
            return 1;
        }

        prompt_text = read_file(prompt_path);
        response_text = read_file(response_path);
        apply_summary = read_file(apply_summary_path);
        debug_state_json = read_file(debug_state_path);
        before_png = read_binary_file(before_path);
        after_png = read_binary_file(after_path);
        if (prompt_text.empty() || response_text.empty() || before_png.empty() || after_png.empty()) {
            std::cerr << "Error: live GPU run did not produce the expected artifacts\n";
            return 1;
        }

        auto parsed_bounds = parse_target_bounds(debug_state_json);
        if (opts.target != "all" && parsed_bounds.valid) {
            target_x = parsed_bounds.x;
            target_y = parsed_bounds.y;
            target_w = parsed_bounds.width;
            target_h = parsed_bounds.height;
            target_found = target_w > 0.0f && target_h > 0.0f;
        }
    } else {
        View root;
        root.set_theme(Theme::dark());
        root.flex().direction = FlexDirection::column;
        root.set_bounds({0.0f, 0.0f, static_cast<float>(opts.width), static_cast<float>(opts.height)});

        StateStore store;
        ScriptEngine engine;
        WidgetBridge bridge(engine, root, store);
        if (!opts.ai_cli.empty()) {
            engine.invoke("setAICli", opts.ai_cli);
        } else if (auto ai_cli = pulp::runtime::get_env("PULP_AI_CLI")) {
            engine.invoke("setAICli", *ai_cli);
        }

        try {
            load_design_tool(opts.script_path, bridge);
        } catch (const std::exception& e) {
            std::cerr << "Error loading design tool: " << e.what() << "\n";
            return 1;
        }
        root.layout_children();

        try {
            if (opts.target != "all") {
                engine.invoke("setDesignDebugTarget", opts.target);
                root.layout_children();
            } else {
                engine.invoke("clearInspectedComponent");
                root.layout_children();
            }
            engine.invoke("setDesignDebugAIConfig", opts.provider, opts.model, opts.reasoning_effort);
            root.layout_children();
        } catch (const std::exception& e) {
            std::cerr << "Error setting debug state: " << e.what() << "\n";
            return 1;
        }

        before_png = render_to_png(root, opts.width, opts.height, opts.scale, opts.capture_backend);
        if (before_png.empty() || !write_binary_file(before_path, before_png)) {
            std::cerr << "Error: failed to render before screenshot\n";
            return 1;
        }

        try {
            prompt_text = engine.invoke("buildDesignChatPrompt", opts.prompt).toString();
        } catch (const std::exception& e) {
            std::cerr << "Error building design prompt: " << e.what() << "\n";
            return 1;
        }
        if (!write_text_file(prompt_path, prompt_text)) {
            std::cerr << "Error: failed to write prompt file\n";
            return 1;
        }

        if (!opts.response_file.empty()) {
            response_text = read_file(opts.response_file);
        } else {
            auto temp_prompt = fs::temp_directory_path() / (stem + "-prompt.txt");
            if (!write_text_file(temp_prompt, prompt_text)) {
                std::cerr << "Error: failed to write temp prompt file\n";
                return 1;
            }
            try {
                command_string = engine.invoke("buildAiCliCommand",
                                               temp_prompt.string(),
                                               opts.model,
                                               opts.provider,
                                               opts.reasoning_effort).toString();
            } catch (const std::exception& e) {
                std::cerr << "Error building AI CLI command: " << e.what() << "\n";
                return 1;
            }
            response_text = run_command_capture(command_string, command_exit);
            if (command_exit != 0) {
                std::cerr << "AI command failed with exit code " << command_exit << "\n";
                if (!response_text.empty()) std::cerr << response_text << "\n";
                return 1;
            }
        }
        if (!write_text_file(response_path, response_text)) {
            std::cerr << "Error: failed to write response file\n";
            return 1;
        }

        try {
            apply_summary = engine.invoke("applyDesignChatResponse", response_text).toString();
            root.layout_children();
        } catch (const std::exception& e) {
            std::cerr << "Error applying design response: " << e.what() << "\n";
            return 1;
        }

        after_png = render_to_png(root, opts.width, opts.height, opts.scale, opts.capture_backend);
        if (after_png.empty() || !write_binary_file(after_path, after_png)) {
            std::cerr << "Error: failed to render after screenshot\n";
            return 1;
        }

        try {
            debug_state_json = engine.invoke("getDesignDebugStateJson").toString();
        } catch (...) {
        }

        if (!write_text_file(debug_state_path, debug_state_json)) {
            std::cerr << "Error: failed to write debug state file\n";
            return 1;
        }
        if (!write_text_file(apply_summary_path, apply_summary)) {
            std::cerr << "Error: failed to write apply summary file\n";
            return 1;
        }

        if (opts.target != "all") {
            if (auto* target = bridge.widget(opts.target)) {
                auto b = target->bounds();
                float abs_x = 0.0f;
                float abs_y = 0.0f;
                for (View* v = target; v != nullptr; v = v->parent()) {
                    abs_x += v->bounds().x;
                    abs_y += v->bounds().y;
                }
                target_x = abs_x;
                target_y = abs_y;
                target_w = b.width;
                target_h = b.height;
                target_found = true;
            }
        }
    }

    auto diff_png = generate_diff_image(before_png, after_png);
    if (!diff_png.empty()) {
        write_binary_file(diff_path, diff_png);
    }
    auto compare = compare_screenshots(before_png, after_png);
    auto changed_bounds = diff_bounds(before_png, after_png);

    TargetArtifacts target_artifacts;
    if (opts.target != "all") {
        const auto padding = static_cast<uint32_t>(std::lround(24.0f * opts.scale));
        uint32_t crop_x = 0;
        uint32_t crop_y = 0;
        uint32_t crop_w = 0;
        uint32_t crop_h = 0;
        bool have_crop = false;

        if (target_found && target_w > 0.0f && target_h > 0.0f && (target_x > 1.0f || target_y > 1.0f)) {
            crop_x = static_cast<uint32_t>(std::max(0.0f, std::floor(target_x * opts.scale) - static_cast<float>(padding)));
            crop_y = static_cast<uint32_t>(std::max(0.0f, std::floor(target_y * opts.scale) - static_cast<float>(padding)));
            crop_w = static_cast<uint32_t>(std::ceil(target_w * opts.scale)) + padding * 2u;
            crop_h = static_cast<uint32_t>(std::ceil(target_h * opts.scale)) + padding * 2u;
            target_artifacts.source = "widget_bounds";
            have_crop = true;
        } else if (changed_bounds.valid) {
            crop_x = changed_bounds.x > padding ? changed_bounds.x - padding : 0u;
            crop_y = changed_bounds.y > padding ? changed_bounds.y - padding : 0u;
            crop_w = changed_bounds.width + padding * 2u;
            crop_h = changed_bounds.height + padding * 2u;
            target_artifacts.source = "changed_pixels";
            have_crop = true;
        }

        if (have_crop) {
            auto before_crop = crop_png(before_png, crop_x, crop_y, crop_w, crop_h);
            auto after_crop = crop_png(after_png, crop_x, crop_y, crop_w, crop_h);
            if (!before_crop.empty() && !after_crop.empty()) {
                target_artifacts.before_path = opts.output_dir / (stem + "-target-before.png");
                target_artifacts.after_path = opts.output_dir / (stem + "-target-after.png");
                target_artifacts.diff_path = opts.output_dir / (stem + "-target-diff.png");
                target_artifacts.x = crop_x;
                target_artifacts.y = crop_y;
                target_artifacts.width = crop_w;
                target_artifacts.height = crop_h;
                target_artifacts.padding = padding;

                if (write_binary_file(target_artifacts.before_path, before_crop) &&
                    write_binary_file(target_artifacts.after_path, after_crop)) {
                    auto target_diff_png = generate_diff_image(before_crop, after_crop);
                    if (!target_diff_png.empty()) {
                        target_artifacts.diff_written = write_binary_file(target_artifacts.diff_path, target_diff_png);
                    }
                    target_artifacts.compare = compare_screenshots(before_crop, after_crop);
                    target_artifacts.valid = target_artifacts.compare.valid;
                }
            }
        }
    }

    std::ostringstream report;
    report << "{\n";
    report << "  \"tool\": \"pulp-design-debug\",\n";
    report << "  \"timestamp\": \"" << json_escape(stamp) << "\",\n";
    report << "  \"script\": \"" << json_escape(opts.script_path.string()) << "\",\n";
    report << "  \"render_backend\": \"" << capture_mode_report_name(opts.capture_mode, opts.capture_backend) << "\",\n";
    report << "  \"requested_capture_backend\": \"" << capture_mode_flag_name(opts.capture_mode, opts.capture_backend) << "\",\n";
    report << "  \"sksl_gpu_supported\": " << (opts.capture_mode == CaptureMode::live_gpu ? "true" : "false") << ",\n";
    report << "  \"widget_sksl_render_supported\": " << (capture_mode_supports_widget_sksl(opts.capture_mode, opts.capture_backend) ? "true" : "false") << ",\n";
    report << "  \"warnings\": [\n";
    if (opts.capture_mode == CaptureMode::live_gpu) {
        report << "    \"This run used the live GPU design tool automation path, so the before/after artifacts reflect the real Skia/Graphite widget render path instead of the offscreen headless fallback.\"\n";
    } else if (opts.capture_backend == ScreenshotBackend::skia) {
        report << "    \"This run used the offscreen Skia backend, so widget SkSL is rendered in the artifact images. It still does not prove final live GPU presentation parity. Use the interactive design tool for final visual QA.\"\n";
    } else {
        report << "    \"This run used the CoreGraphics headless backend. Widget SkSL is not faithfully rendered here; use this report to validate prompt/response/apply flow and visual diffs, but judge final widget shader quality with Skia-backed capture or the interactive design tool.\"\n";
    }
    report << "  ],\n";
    report << "  \"provider\": \"" << json_escape(opts.provider) << "\",\n";
    report << "  \"model\": \"" << json_escape(opts.model) << "\",\n";
    report << "  \"reasoning_effort\": \"" << json_escape(opts.reasoning_effort) << "\",\n";
    report << "  \"target\": \"" << json_escape(opts.target) << "\",\n";
    report << "  \"target_found\": " << (target_found ? "true" : "false") << ",\n";
    report << "  \"target_bounds\": {\"x\": " << target_x << ", \"y\": " << target_y
           << ", \"width\": " << target_w << ", \"height\": " << target_h << "},\n";
    report << "  \"prompt\": \"" << json_escape(opts.prompt) << "\",\n";
    report << "  \"prompt_file\": \"" << json_escape(prompt_path.string()) << "\",\n";
    report << "  \"response_file\": \"" << json_escape(response_path.string()) << "\",\n";
    report << "  \"debug_state_file\": \"" << json_escape(debug_state_path.string()) << "\",\n";
    report << "  \"apply_summary_file\": \"" << json_escape(apply_summary_path.string()) << "\",\n";
    report << "  \"before_image\": \"" << json_escape(before_path.string()) << "\",\n";
    report << "  \"after_image\": \"" << json_escape(after_path.string()) << "\",\n";
    report << "  \"diff_image\": \"" << json_escape(diff_path.string()) << "\",\n";
    report << "  \"similarity_pct\": " << static_cast<int>(compare.similarity * 100.0f) << ",\n";
    report << "  \"diff_pixels\": " << compare.diff_pixels << ",\n";
    report << "  \"total_pixels\": " << compare.total_pixels << ",\n";
    report << "  \"diff_pct\": " << (compare.total_pixels > 0 ? (100.0 * static_cast<double>(compare.diff_pixels) / static_cast<double>(compare.total_pixels)) : 0.0) << ",\n";
    report << "  \"mean_error\": " << compare.mean_error << ",\n";
    report << "  \"target_region\": {\"x\": " << target_artifacts.x << ", \"y\": " << target_artifacts.y
           << ", \"width\": " << target_artifacts.width << ", \"height\": " << target_artifacts.height
           << ", \"padding\": " << target_artifacts.padding << "},\n";
    report << "  \"target_region_source\": \"" << json_escape(target_artifacts.source) << "\",\n";
    if (target_artifacts.valid) {
        report << "  \"target_before_image\": \"" << json_escape(target_artifacts.before_path.string()) << "\",\n";
        report << "  \"target_after_image\": \"" << json_escape(target_artifacts.after_path.string()) << "\",\n";
        if (target_artifacts.diff_written) {
            report << "  \"target_diff_image\": \"" << json_escape(target_artifacts.diff_path.string()) << "\",\n";
        } else {
            report << "  \"target_diff_image\": null,\n";
        }
        report << "  \"target_similarity_pct\": " << static_cast<int>(target_artifacts.compare.similarity * 100.0f) << ",\n";
        report << "  \"target_diff_pixels\": " << target_artifacts.compare.diff_pixels << ",\n";
        report << "  \"target_total_pixels\": " << target_artifacts.compare.total_pixels << ",\n";
        report << "  \"target_diff_pct\": " << (target_artifacts.compare.total_pixels > 0 ? (100.0 * static_cast<double>(target_artifacts.compare.diff_pixels) / static_cast<double>(target_artifacts.compare.total_pixels)) : 0.0) << ",\n";
        report << "  \"target_mean_error\": " << target_artifacts.compare.mean_error << ",\n";
        report << "  \"target_region_changed\": " << (target_artifacts.compare.diff_pixels > 0 ? "true" : "false") << ",\n";
    } else {
        report << "  \"target_before_image\": null,\n";
        report << "  \"target_after_image\": null,\n";
        report << "  \"target_diff_image\": null,\n";
        report << "  \"target_similarity_pct\": null,\n";
        report << "  \"target_diff_pixels\": null,\n";
        report << "  \"target_total_pixels\": null,\n";
        report << "  \"target_diff_pct\": null,\n";
        report << "  \"target_mean_error\": null,\n";
        report << "  \"target_region_changed\": null,\n";
    }
    report << "  \"apply_summary\": \"" << json_escape(apply_summary) << "\",\n";
    report << "  \"debug_state\": " << debug_state_json;
    if (!command_string.empty()) {
        if (opts.capture_mode == CaptureMode::live_gpu) {
            report << ",\n  \"driver_command\": \"" << json_escape(command_string) << "\"";
            if (!command_output.empty()) {
                report << ",\n  \"driver_output\": \"" << json_escape(command_output) << "\"";
            }
        } else {
            report << ",\n  \"ai_command\": \"" << json_escape(command_string) << "\"";
        }
    }
    report << "\n}\n";

    if (!write_text_file(report_path, report.str())) {
        std::cerr << "Error: failed to write report file\n";
        return 1;
    }
    write_text_file(latest_report_path, report.str());

    std::ostringstream run_summary;
    run_summary << "{";
    run_summary << "\"timestamp\":\"" << json_escape(stamp) << "\",";
    run_summary << "\"prompt\":\"" << json_escape(opts.prompt) << "\",";
    run_summary << "\"target\":\"" << json_escape(opts.target) << "\",";
    run_summary << "\"provider\":\"" << json_escape(opts.provider) << "\",";
    run_summary << "\"model\":\"" << json_escape(opts.model) << "\",";
    run_summary << "\"reasoning_effort\":\"" << json_escape(opts.reasoning_effort) << "\",";
    run_summary << "\"render_backend\":\"" << capture_mode_report_name(opts.capture_mode, opts.capture_backend) << "\",";
    run_summary << "\"widget_sksl_render_supported\":" << (capture_mode_supports_widget_sksl(opts.capture_mode, opts.capture_backend) ? "true" : "false") << ",";
    run_summary << "\"similarity_pct\":" << static_cast<int>(compare.similarity * 100.0f) << ",";
    run_summary << "\"diff_pct\":" << (compare.total_pixels > 0 ? (100.0 * static_cast<double>(compare.diff_pixels) / static_cast<double>(compare.total_pixels)) : 0.0) << ",";
    if (target_artifacts.valid) {
        run_summary << "\"target_similarity_pct\":" << static_cast<int>(target_artifacts.compare.similarity * 100.0f) << ",";
        run_summary << "\"target_diff_pct\":" << (target_artifacts.compare.total_pixels > 0 ? (100.0 * static_cast<double>(target_artifacts.compare.diff_pixels) / static_cast<double>(target_artifacts.compare.total_pixels)) : 0.0) << ",";
        run_summary << "\"target_region_changed\":" << (target_artifacts.compare.diff_pixels > 0 ? "true" : "false") << ",";
    } else {
        run_summary << "\"target_similarity_pct\":null,";
        run_summary << "\"target_diff_pct\":null,";
        run_summary << "\"target_region_changed\":null,";
    }
    run_summary << "\"apply_summary\":\"" << json_escape(apply_summary) << "\",";
    run_summary << "\"report\":\"" << json_escape(report_path.string()) << "\",";
    run_summary << "\"debug_state_file\":\"" << json_escape(debug_state_path.string()) << "\",";
    run_summary << "\"apply_summary_file\":\"" << json_escape(apply_summary_path.string()) << "\",";
    run_summary << "\"before_image\":\"" << json_escape(before_path.string()) << "\",";
    run_summary << "\"after_image\":\"" << json_escape(after_path.string()) << "\",";
    run_summary << "\"diff_image\":\"" << json_escape(diff_path.string()) << "\"";
    if (target_artifacts.valid) {
        run_summary << ",\"target_before_image\":\"" << json_escape(target_artifacts.before_path.string()) << "\"";
        run_summary << ",\"target_after_image\":\"" << json_escape(target_artifacts.after_path.string()) << "\"";
        if (target_artifacts.diff_written) {
            run_summary << ",\"target_diff_image\":\"" << json_escape(target_artifacts.diff_path.string()) << "\"";
        }
    }
    run_summary << "}\n";
    write_text_file(latest_run_path, run_summary.str());
    append_text_file(runs_path, run_summary.str());

    std::cout << "Prompt saved → " << prompt_path << "\n";
    std::cout << "Before → " << before_path << "\n";
    std::cout << "After → " << after_path << "\n";
    if (!diff_png.empty()) std::cout << "Diff → " << diff_path << "\n";
    if (target_artifacts.valid) {
        std::cout << "Target before → " << target_artifacts.before_path << "\n";
        std::cout << "Target after → " << target_artifacts.after_path << "\n";
        if (target_artifacts.diff_written) std::cout << "Target diff → " << target_artifacts.diff_path << "\n";
    }
    std::cout << "Response → " << response_path << "\n";
    std::cout << "Report → " << report_path << "\n";
    std::cout << "Latest report → " << latest_report_path << "\n";
    std::cout << "Applied → " << apply_summary << "\n";
    std::cout << "Visual change: " << static_cast<int>(compare.similarity * 100.0f)
              << "% similarity (" << compare.diff_pixels << "/" << compare.total_pixels
              << " pixels differ)\n";
    if (target_artifacts.valid) {
        std::cout << "Target change: " << static_cast<int>(target_artifacts.compare.similarity * 100.0f)
                  << "% similarity (" << target_artifacts.compare.diff_pixels << "/"
                  << target_artifacts.compare.total_pixels << " pixels differ)\n";
    }
    return 0;
}
