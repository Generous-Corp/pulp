// Pulp Style Designer — JS-defined design tool
// Minimal C++ host: opens a window, loads JS via ScriptEngine + WidgetBridge,
// hot-reloads on file changes. All UI defined in design-tool.js.

#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/window_host.hpp>
#include <pulp/view/hot_reload.hpp>
#include <pulp/state/store.hpp>
#include <pulp/runtime/system.hpp>
#include <filesystem>
#include <fstream>
#include <memory>
#include <iostream>
#include <sstream>
#include <array>
#include <unordered_map>
#include <cstdlib>
#include <cstdio>
#include <dispatch/dispatch.h>

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

using namespace pulp::view;
using namespace pulp::state;
namespace fs = std::filesystem;

static std::string read_file(const fs::path& path) {
    std::ifstream f(path);
    if (!f.is_open()) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static bool write_text_file(const fs::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;
    out << content;
    return out.good();
}

static bool write_binary_file(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

static std::string run_command_capture(const std::string& command, int& exit_code) {
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

static fs::path find_default_js_path() {
    auto dir = fs::current_path();
    while (!dir.empty()) {
        auto candidate = dir / "examples" / "design-tool" / "design-tool.js";
        if (fs::exists(candidate)) return candidate;
        auto parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return {};
}

struct AutomationOptions {
    bool enabled = false;
    std::string prompt;
    std::string target = "all";
    std::string provider = "claude";
    std::string model = "claude-sonnet-4-6";
    std::string reasoning_effort;
    std::string ai_cli;
    fs::path response_file;
    fs::path before_path;
    fs::path after_path;
    fs::path prompt_out;
    fs::path response_out;
    fs::path debug_state_out;
    fs::path apply_summary_out;
    uint64_t delay_ms = 350;
    uint64_t after_delay_ms = 350;
};

static void print_usage() {
    std::cerr << "Usage: pulp-design-tool [path/to/design-tool.js] [options]\n";
    std::cerr << "  --script <file.js>                 Explicit design-tool.js path\n";
    std::cerr << "  --automation-prompt <text>         Run a one-shot automated chat restyle\n";
    std::cerr << "  --automation-target <id|all>       Target widget for automation (default: all)\n";
    std::cerr << "  --automation-provider <name>       Metadata provider name (default: claude)\n";
    std::cerr << "  --automation-model <id>            Metadata model id (default: claude-sonnet-4-6)\n";
    std::cerr << "  --automation-reasoning-effort <l>  Metadata effort (low|medium|high|xhigh)\n";
    std::cerr << "  --automation-ai-cli <template>     Override AI CLI command template\n";
    std::cerr << "  --automation-response-file <file>  Use a saved model response instead of calling AI\n";
    std::cerr << "  --automation-before <file.png>     Save live before screenshot\n";
    std::cerr << "  --automation-after <file.png>      Save live after screenshot\n";
    std::cerr << "  --automation-prompt-out <file>     Save generated prompt text\n";
    std::cerr << "  --automation-response-out <file>   Save model response text\n";
    std::cerr << "  --automation-debug-state-out <f>   Save getDesignDebugStateJson() output\n";
    std::cerr << "  --automation-apply-summary-out <f> Save applyDesignChatResponse() summary\n";
    std::cerr << "  --automation-delay-ms <ms>         Delay before baseline capture (default: 350)\n";
    std::cerr << "  --automation-after-delay-ms <ms>   Delay before post-apply capture (default: 350)\n";
}

int main(int argc, char* argv[]) {
    fs::path js_path;
    AutomationOptions automation;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--script" && i + 1 < argc) {
            js_path = argv[++i];
        } else if (arg == "--automation-prompt" && i + 1 < argc) {
            automation.enabled = true;
            automation.prompt = argv[++i];
        } else if (arg == "--automation-target" && i + 1 < argc) {
            automation.enabled = true;
            automation.target = argv[++i];
        } else if (arg == "--automation-provider" && i + 1 < argc) {
            automation.enabled = true;
            automation.provider = argv[++i];
        } else if (arg == "--automation-model" && i + 1 < argc) {
            automation.enabled = true;
            automation.model = argv[++i];
        } else if (arg == "--automation-reasoning-effort" && i + 1 < argc) {
            automation.enabled = true;
            automation.reasoning_effort = argv[++i];
        } else if (arg == "--automation-ai-cli" && i + 1 < argc) {
            automation.enabled = true;
            automation.ai_cli = argv[++i];
        } else if (arg == "--automation-response-file" && i + 1 < argc) {
            automation.enabled = true;
            automation.response_file = argv[++i];
        } else if (arg == "--automation-before" && i + 1 < argc) {
            automation.enabled = true;
            automation.before_path = argv[++i];
        } else if (arg == "--automation-after" && i + 1 < argc) {
            automation.enabled = true;
            automation.after_path = argv[++i];
        } else if (arg == "--automation-prompt-out" && i + 1 < argc) {
            automation.enabled = true;
            automation.prompt_out = argv[++i];
        } else if (arg == "--automation-response-out" && i + 1 < argc) {
            automation.enabled = true;
            automation.response_out = argv[++i];
        } else if (arg == "--automation-debug-state-out" && i + 1 < argc) {
            automation.enabled = true;
            automation.debug_state_out = argv[++i];
        } else if (arg == "--automation-apply-summary-out" && i + 1 < argc) {
            automation.enabled = true;
            automation.apply_summary_out = argv[++i];
        } else if (arg == "--automation-delay-ms" && i + 1 < argc) {
            automation.enabled = true;
            automation.delay_ms = static_cast<uint64_t>(std::stoull(argv[++i]));
        } else if (arg == "--automation-after-delay-ms" && i + 1 < argc) {
            automation.enabled = true;
            automation.after_delay_ms = static_cast<uint64_t>(std::stoull(argv[++i]));
        } else if ((arg == "--help" || arg == "-h")) {
            print_usage();
            return 0;
        } else if (!arg.empty() && arg[0] != '-' && js_path.empty()) {
            js_path = arg;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage();
            return 1;
        }
    }

    if (js_path.empty()) {
        js_path = find_default_js_path();
    }

    if (js_path.empty() || !fs::exists(js_path)) {
        std::cerr << "Error: design-tool.js not found.\n";
        print_usage();
        return 1;
    }
    if (automation.enabled && automation.prompt.empty()) {
        std::cerr << "Error: --automation-prompt is required when automation is enabled.\n";
        return 1;
    }
    if (!automation.response_file.empty() && !fs::exists(automation.response_file)) {
        std::cerr << "Error: automation response file not found: " << automation.response_file << "\n";
        return 1;
    }

    std::cout << "Loading: " << js_path.string() << "\n";

    // Create the view tree root
    View root;
    root.set_theme(Theme::dark());
    root.flex().direction = FlexDirection::column;

    StateStore store;
    auto make_engine = [](std::string prefix = {}) {
        auto engine = std::make_unique<ScriptEngine>();
        engine->set_log_callback([prefix = std::move(prefix)](std::string_view level, std::string_view msg) {
            if (!prefix.empty()) {
                std::cerr << prefix << "[" << level << "] " << msg << "\n";
            } else {
                std::cerr << "[" << level << "] " << msg << "\n";
            }
        });
        return engine;
    };

    auto engine = make_engine();
    auto bridge = std::make_unique<WidgetBridge>(*engine, root, store);
    if (!automation.ai_cli.empty()) {
        bridge->set_ai_cli_command(automation.ai_cli);
    } else if (auto ai_cli = pulp::runtime::get_env("PULP_AI_CLI")) {
        bridge->set_ai_cli_command(*ai_cli);
    }

    bridge->install_runtime_import_handlers();

    // Load library scripts first (oklch.js)
    auto js_dir = js_path.parent_path();
    auto load_library_scripts = [&](WidgetBridge& target) {
        for (auto& lib : {"oklch.js"}) {
            auto lib_path = js_dir / lib;
            if (fs::exists(lib_path)) {
                target.load_script(read_file(lib_path));
            }
        }
    };
    try {
        load_library_scripts(*bridge);
        std::cout << "Loaded: oklch.js\n";
    } catch (const std::exception& e) {
        std::cerr << "Error loading library scripts: " << e.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "Error loading library scripts: unknown exception\n";
        return 1;
    }

    // Load main UI script
    auto code = read_file(js_path);
    if (code.empty()) {
        std::cerr << "Error: could not read " << js_path.string() << "\n";
        return 1;
    }
    try {
        bridge->load_script(code);
    } catch (const std::exception& e) {
        std::cerr << "Error loading design tool script: " << e.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "Error loading design tool script: unknown exception\n";
        return 1;
    }

    std::cout << "UI created: " << root.child_count() << " top-level views\n";

    // Open window
    WindowOptions opts;
    opts.title = "Pulp Style Designer";
    opts.width = 1100;
    opts.height = 700;
    opts.min_width = 1000;  // Issue 5: prevent scrollbar overlap
    opts.min_height = 600;
    opts.resizable = true;
    opts.use_gpu = true;

    auto window = WindowHost::create(root, opts);
    bridge->set_repaint_callback([&window] {
        if (window) window->repaint();
    });
    window->set_close_callback([] {
        std::cout << "Window closed\n";
    });

    // Hot reload
    HotReloader reloader(js_path, [&](const std::string& new_code) {
        std::cout << "Hot reload: " << js_path.filename().string() << "\n";
        try {
            // Validate the new script in an isolated bridge first so a bad
            // reload does not tear down the live UI.
            View probe_root;
            probe_root.set_theme(root.theme());
            probe_root.flex().direction = FlexDirection::column;
            StateStore probe_store;
            auto probe_engine = make_engine("reload:");
            auto probe_bridge = std::make_unique<WidgetBridge>(*probe_engine, probe_root, probe_store);
            probe_bridge->install_runtime_import_handlers();
            load_library_scripts(*probe_bridge);
            probe_bridge->load_script(new_code);

            WidgetReloadSnapshot saved;
            bridge->snapshot_values(saved);
            window->invalidate_input_state();
            bridge->clear();

            auto next_engine = make_engine();
            auto next_bridge = std::make_unique<WidgetBridge>(*next_engine, root, store);
            if (!automation.ai_cli.empty()) {
                next_bridge->set_ai_cli_command(automation.ai_cli);
            } else if (auto ai_cli = pulp::runtime::get_env("PULP_AI_CLI")) {
                next_bridge->set_ai_cli_command(*ai_cli);
            }
            next_bridge->install_runtime_import_handlers();
            load_library_scripts(*next_bridge);
            next_bridge->load_script(new_code);
            next_bridge->restore_values(saved);
            engine = std::move(next_engine);
            bridge = std::move(next_bridge);
            bridge->set_repaint_callback([&window] {
                if (window) window->repaint();
            });
            window->repaint();
        } catch (const std::exception& e) {
            std::cerr << "Hot reload failed: " << e.what() << "\n";
        } catch (...) {
            std::cerr << "Hot reload failed: unknown exception\n";
        }
    });

    // Poll hot-reload and async bridge results on a GCD timer (main thread).
    auto* reload_timer = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(reload_timer,
        dispatch_time(DISPATCH_TIME_NOW, 0),
        100 * NSEC_PER_MSEC, 20 * NSEC_PER_MSEC);
    auto* reloader_ptr = &reloader;
    auto* bridge_slot = &bridge;
    dispatch_source_set_event_handler(reload_timer, ^{
        try {
            if (*bridge_slot) {
                (*bridge_slot)->poll_async_results();
            }
            reloader_ptr->poll_reload();
        } catch (const std::exception& e) {
            std::cerr << "Design tool event loop error: " << e.what() << "\n";
        } catch (...) {
            std::cerr << "Design tool event loop error: unknown exception\n";
        }
    });
    dispatch_resume(reload_timer);

    {
        auto* bridge_slot = &bridge;
        auto* window_ptr = window.get();
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 200 * NSEC_PER_MSEC),
                       dispatch_get_main_queue(), ^{
            if (*bridge_slot) {
                try {
                    (*bridge_slot)->load_script(
                        "if (typeof __pulpRuntimeSettle__ === 'function') __pulpRuntimeSettle__(4);");
                } catch (...) {
                }
                if (window_ptr) window_ptr->repaint();
            }
        });
    }

    int automation_exit_code = 0;
    if (automation.enabled) {
        auto automation_copy = automation;
        auto* bridge_slot = &bridge;
        auto* engine_slot = &engine;
        auto* automation_exit_code_ptr = &automation_exit_code;
        auto* window_ptr = window.get();
        auto* root_ptr = &root;

        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(automation_copy.delay_ms) * NSEC_PER_MSEC),
                       dispatch_get_main_queue(), ^{
            auto fail = [&](const std::string& message) {
                std::cerr << "Automation failed: " << message << "\n";
                *automation_exit_code_ptr = 1;
                if (window_ptr) window_ptr->request_close();
            };

            try {
                if (!*engine_slot || !*bridge_slot || !window_ptr) {
                    fail("automation host not ready");
                    return;
                }

                if (automation_copy.target != "all") {
                    (*engine_slot)->invoke("setDesignDebugTarget", automation_copy.target);
                } else {
                    (*engine_slot)->invoke("clearInspectedComponent");
                }
                (*engine_slot)->invoke("setDesignDebugAIConfig",
                                       automation_copy.provider,
                                       automation_copy.model,
                                       automation_copy.reasoning_effort);
                root_ptr->layout_children();
                window_ptr->repaint();

                if (!automation_copy.before_path.empty()) {
                    fs::create_directories(automation_copy.before_path.parent_path());
                    auto before_png = window_ptr->capture_png();
                    if (before_png.empty() || !write_binary_file(automation_copy.before_path, before_png)) {
                        fail("failed to capture before image");
                        return;
                    }
                }

                auto prompt_text = (*engine_slot)->invoke("buildDesignChatPrompt", automation_copy.prompt).toString();
                if (!automation_copy.prompt_out.empty()) {
                    fs::create_directories(automation_copy.prompt_out.parent_path());
                    if (!write_text_file(automation_copy.prompt_out, prompt_text)) {
                        fail("failed to write prompt output");
                        return;
                    }
                }

                std::string response_text;
                if (!automation_copy.response_file.empty()) {
                    response_text = read_file(automation_copy.response_file);
                } else {
                    auto temp_prompt = fs::temp_directory_path() / "pulp-design-tool-automation-prompt.txt";
                    if (!write_text_file(temp_prompt, prompt_text)) {
                        fail("failed to write temp prompt");
                        return;
                    }
                    std::string command_string;
                    try {
                        command_string = (*engine_slot)->invoke("buildAiCliCommand",
                                                                temp_prompt.string(),
                                                                automation_copy.model,
                                                                automation_copy.provider,
                                                                automation_copy.reasoning_effort).toString();
                    } catch (const std::exception& e) {
                        fs::remove(temp_prompt);
                        fail(std::string("failed to build AI command: ") + e.what());
                        return;
                    }
                    int exit_code = 0;
                    response_text = run_command_capture(command_string, exit_code);
                    fs::remove(temp_prompt);
                    if (exit_code != 0) {
                        fail("AI command failed with exit code " + std::to_string(exit_code));
                        return;
                    }
                }

                if (!automation_copy.response_out.empty()) {
                    fs::create_directories(automation_copy.response_out.parent_path());
                    if (!write_text_file(automation_copy.response_out, response_text)) {
                        fail("failed to write response output");
                        return;
                    }
                }

                auto apply_summary = (*engine_slot)->invoke("applyDesignChatResponse", response_text).toString();
                if (!automation_copy.apply_summary_out.empty()) {
                    fs::create_directories(automation_copy.apply_summary_out.parent_path());
                    if (!write_text_file(automation_copy.apply_summary_out, apply_summary)) {
                        fail("failed to write apply summary output");
                        return;
                    }
                }

                root_ptr->layout_children();
                window_ptr->repaint();

                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(automation_copy.after_delay_ms) * NSEC_PER_MSEC),
                               dispatch_get_main_queue(), ^{
                    try {
                        if (!automation_copy.after_path.empty()) {
                            fs::create_directories(automation_copy.after_path.parent_path());
                            auto after_png = window_ptr->capture_png();
                            if (after_png.empty() || !write_binary_file(automation_copy.after_path, after_png)) {
                                fail("failed to capture after image");
                                return;
                            }
                        }

                        if (!automation_copy.debug_state_out.empty()) {
                            fs::create_directories(automation_copy.debug_state_out.parent_path());
                            auto debug_state = (*engine_slot)->invoke("getDesignDebugStateJson").toString();
                            if (!write_text_file(automation_copy.debug_state_out, debug_state)) {
                                fail("failed to write debug state output");
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
                    window_ptr->request_close();
                });
            } catch (const std::exception& e) {
                fail(std::string("automation error: ") + e.what());
            } catch (...) {
                fail("automation error");
            }
        });
    }

    std::cout << "Hot reload watching: " << js_path.parent_path().string() << "\n";
    std::cout << "Opening window...\n";

    window->run_event_loop();
    dispatch_source_cancel(reload_timer);
    return automation_exit_code;
}
