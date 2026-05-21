// Pulp Style Designer — JS-defined design tool
// Minimal C++ host: opens a window, loads JS via ScriptEngine + WidgetBridge,
// hot-reloads on file changes. The UI is split by concern across the
// design-tool-*.js modules (see kDesignToolModules below). They share one
// global scope and MUST load in the listed order so each declaration
// precedes its first use — the concatenation of the modules in this order
// is byte-equivalent to the historical single-file design-tool.js.

#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/window_host.hpp>
#include <pulp/view/hot_reload.hpp>
#include <pulp/view/viewport_reconcile.hpp>
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

// Ordered design-tool concern modules. Loaded sequentially into one shared
// global scope; the concatenation in this order equals the historical
// single-file design-tool.js, so declaration-before-use order is preserved.
static const char* const kDesignToolModules[] = {
    "design-tool-core.js",     // theme bootstrap, frame shim, token state/color,
                               // APP_* constants, preview-theme base
    "design-tool-toolbar.js",  // root shell, toolbar, token list
    "design-tool-palette.js",  // OKLCH shade ramps, gamut, accent hue,
                               // save/load, legacy color picker
    "design-tool-popup.js",    // token palette popup, gamut triangle,
                               // accent-hue + harmony selector wiring
    "design-tool-preview.js",  // preview chrome, controls, waveforms,
                               // layout cards, right-panel chat scaffold
    "design-tool-chat.js",     // AI CLI, chat controls, inspector, global
                               // keys, shader presets, chat input wiring
    "design-tool-export.js",   // export popup, undo/redo wiring
};

// The directory-watch anchor / default entry. Picking the first module keeps
// the historical "give the host a single .js path" CLI contract intact.
static const char* const kDesignToolEntry = "design-tool-core.js";

static fs::path find_default_js_path() {
    auto dir = fs::current_path();
    while (!dir.empty()) {
        auto candidate = dir / "examples" / "design-tool" / kDesignToolEntry;
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
    std::cerr << "Usage: pulp-design-tool [path/to/design-tool-core.js] [options]\n";
    std::cerr << "  --script <file.js>                 Explicit entry-module path (its\n";
    std::cerr << "                                     directory must hold all design-tool-*.js)\n";
    std::cerr << "  --design-width <px>                Override design viewport width (else auto-measure)\n";
    std::cerr << "  --design-height <px>               Override design viewport height (else auto-measure)\n";
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
    std::cerr << "  --no-show-window                   Run with no visible window (live-host smoke / CI)\n";
    std::cerr << "  --exit-after-ms <N>                Auto request_close() after N ms (live-host smoke / CI)\n";
}

int main(int argc, char* argv[]) {
    // pulp-internal #70 — defense-in-depth GPU host check. The CMake
    // build gate at examples/design-tool/CMakeLists.txt should refuse
    // to configure without PULP_HAS_SKIA, but the same check repeated
    // here at startup catches the case where someone built with
    // -DPULP_ENABLE_GPU=OFF or hand-edited the example out of the
    // mandatory list. Without Skia/Dawn the design-viewport and
    // aspect-lock are no-ops (set_design_viewport / set_fixed_aspect_
    // ratio are base-class no-ops) and the user gets a broken UX with
    // no obvious cause — burned ~3h of session in 2026-05.
#ifndef PULP_HAS_SKIA
    std::cerr <<
        "FATAL: pulp-design-tool was built without PULP_HAS_SKIA. The GPU "
        "host (MacGpuWindowHost) is required for the design-viewport pin "
        "and aspect-locked resize that this tool's UX depends on. Without "
        "it, content does not scale and the dark fill is exposed past the "
        "design surface. Rebuild with Skia linked. See "
        "planning/2026-05-19-gpu-host-fallback.md for the recovery recipe.\n";
    return 78;  // EX_CONFIG
#endif

    fs::path js_path;
    AutomationOptions automation;
    uint64_t exit_after_ms = 0;     // pulp-internal #71 — 0 = disabled (normal interactive use)
    bool no_show_window = false;    // pulp-internal #71 — true = skip Dock icon + window display
    // pulp-internal #70 — design viewport sizing. Operator-level CLI
    // overrides; if both stay 0 the size is read from JS metadata,
    // auto-measured from the view tree, or falls back to 1320×860.
    float cli_design_w = 0.0f;
    float cli_design_h = 0.0f;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--script" && i + 1 < argc) {
            js_path = argv[++i];
        } else if (arg == "--design-width" && i + 1 < argc) {
            cli_design_w = static_cast<float>(std::atof(argv[++i]));
        } else if (arg == "--design-height" && i + 1 < argc) {
            cli_design_h = static_cast<float>(std::atof(argv[++i]));
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
        } else if (arg == "--no-show-window") {
            no_show_window = true;
        } else if (arg == "--exit-after-ms" && i + 1 < argc) {
            exit_after_ms = static_cast<uint64_t>(std::stoull(argv[++i]));
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

    const bool used_default_js_path = js_path.empty();
    if (js_path.empty()) {
        js_path = find_default_js_path();
    }

    if (js_path.empty() || !fs::exists(js_path)) {
        std::cerr << "Error: " << kDesignToolEntry << " not found.\n";
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

    // Load the OKLCH library, then the design-tool concern modules — all into
    // one shared global scope. Order is load-bearing: oklch.js defines the
    // colour engine the modules call at parse time, and the modules concatenate
    // in kDesignToolModules order to equal the historical single-file script.
    auto js_dir = js_path.parent_path();
    auto load_design_tool = [&](WidgetBridge& target) {
        auto oklch_path = js_dir / "oklch.js";
        if (fs::exists(oklch_path)) {
            target.load_script(read_file(oklch_path));
        }
        for (const char* module : kDesignToolModules) {
            auto module_path = js_dir / module;
            if (!fs::exists(module_path)) {
                throw std::runtime_error(std::string("missing design-tool module: ")
                                         + module_path.string());
            }
            auto module_code = read_file(module_path);
            if (module_code.empty()) {
                throw std::runtime_error(std::string("empty design-tool module: ")
                                         + module_path.string());
            }
            target.load_script(module_code);
        }
    };
    try {
        load_design_tool(*bridge);
        std::cout << "Loaded: oklch.js + " << std::size(kDesignToolModules)
                  << " design-tool modules\n";
    } catch (const std::exception& e) {
        std::cerr << "Error loading design tool scripts: " << e.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "Error loading design tool scripts: unknown exception\n";
        return 1;
    }

    std::cout << "UI created: " << root.child_count() << " top-level views\n";

    // Open window — title derives from the script filename so a user
    // running `pulp-design-tool --script spectr/editor.js` sees "Spectr —
    // editor.js" in the title bar rather than the generic "Pulp Style
    // Designer". pulp-internal #61. Stable identifier: strip extension,
    // keep the immediate parent directory if it looks meaningful (not
    // "dist" / "build" / "out"), fall back to "Pulp Design Tool" only
    // when no script was given.
    WindowOptions opts;
    {
        std::string title;
        if (used_default_js_path) {
            // Default run loads the design-tool-*.js module set; the per-file
            // stem ("design-tool-core") is an implementation detail, so keep
            // the stable product name rather than deriving it from a module.
            title = "Pulp Design Tool";
        } else if (!js_path.empty()) {
            auto p = js_path;
            auto stem = p.stem().string();      // e.g. "editor"
            auto parent = p.parent_path().filename().string();
            // Strip build-output directory names so the title carries the
            // upstream app name, not the artifact location.
            static const std::array<std::string, 5> kIgnored = {
                "dist", "build", "out", "target", "release"};
            bool ignore_parent = false;
            for (const auto& s : kIgnored) {
                if (parent == s) { ignore_parent = true; break; }
            }
            if (ignore_parent || parent.empty()) {
                // Walk up one more level for a meaningful name.
                auto grandparent = p.parent_path().parent_path().filename().string();
                if (!grandparent.empty()) parent = grandparent;
            }
            title = parent.empty() ? stem : (parent + " — " + stem);
        } else {
            title = "Pulp Design Tool";
        }
        opts.title = title;
    }
    // pulp #59/#63/#64/#65 — design-viewport metadata (see
    // WindowHost::set_design_viewport). Spectr's editor.js is authored
    // at 1320x860 with chains of position:absolute+inset:0 children
    // that Yoga can't size into a fill, so per-child scale and JS
    // canvas-refit both fail. The window host instead pins the root
    // at design size and applies an aspect-correct paint-time fit.
    //
    // pulp-internal #70 — auto-measure / metadata-driven viewport.
    // Priority order (first match wins):
    //   1. CLI flag --design-width / --design-height (operator override)
    //   2. PULP_DESIGN_WIDTH / _HEIGHT env vars (CI / scripted runs)
    //   3. globalThis.__pulpDesignViewport__ = { width, height } in JS
    //   4. root.intrinsic_width() / intrinsic_height() (auto-measure)
    //   5. Fallback: 1320 × 860 (Spectr-era default)
    // Each fallback is logged so an operator can tell why a given size
    // was selected without diff-rebuilding.
    constexpr float kFallbackDesignWidth  = 1320.0f;
    constexpr float kFallbackDesignHeight = 860.0f;
    constexpr float kMinSensibleDim       = 100.0f;
    constexpr float kMaxSensibleDim       = 4000.0f;
    auto in_range = [](float v) {
        return v >= kMinSensibleDim && v <= kMaxSensibleDim;
    };

    float design_w = 0.0f, design_h = 0.0f;
    const char* viewport_source = "fallback";
    // Set to true when probe-scan detects flex-wrap / reflow-capable
    // layouts. Responsive designs skip the design-viewport pin and
    // aspect-lock so the JSX can re-wrap naturally as the user
    // resizes; non-responsive (fixed-size) designs keep the pin so
    // resize stays proportional and centered.
    bool is_responsive = false;

    // 1. CLI flag override.
    if (cli_design_w > 0.0f && cli_design_h > 0.0f) {
        design_w = cli_design_w;
        design_h = cli_design_h;
        viewport_source = "--design-width/--design-height";
    }

    // 2. Env-var override (useful for CI sweeps + screenshot harnesses).
    if (design_w <= 0.0f) {
        const char* envw = std::getenv("PULP_DESIGN_WIDTH");
        const char* envh = std::getenv("PULP_DESIGN_HEIGHT");
        if (envw && envh) {
            float vw = static_cast<float>(std::atof(envw));
            float vh = static_cast<float>(std::atof(envh));
            if (vw > 0.0f && vh > 0.0f) {
                design_w = vw;
                design_h = vh;
                viewport_source = "PULP_DESIGN_WIDTH/_HEIGHT";
            }
        }
    }

    // 3. JSX-side declaration via globalThis.__pulpDesignViewport__.
    //    Lets the imported design file specify its target size:
    //      globalThis.__pulpDesignViewport__ = { width: 1280, height: 720 };
    if (design_w <= 0.0f) {
        try {
            auto vp = engine->evaluate(
                "(typeof globalThis.__pulpDesignViewport__ === 'object' "
                "  && globalThis.__pulpDesignViewport__ !== null "
                "  && typeof globalThis.__pulpDesignViewport__.width === 'number' "
                "  && typeof globalThis.__pulpDesignViewport__.height === 'number') "
                "? (globalThis.__pulpDesignViewport__.width + 'x' "
                "   + globalThis.__pulpDesignViewport__.height) "
                ": ''");
            if (vp.isString()) {
                std::string s(vp.getString());
                float vw = 0.0f, vh = 0.0f;
                if (std::sscanf(s.c_str(), "%fx%f", &vw, &vh) == 2
                    && vw > 0.0f && vh > 0.0f) {
                    design_w = vw;
                    design_h = vh;
                    viewport_source = "globalThis.__pulpDesignViewport__";
                }
            }
        } catch (...) {
            // engine error — ignore and fall through to auto-measure
        }
    }

    // 4. Probe-layout from the view tree built by the JS script.
    //    Strategy: find the narrowest width at which the layout
    //    "settles" (height stops shrinking) — that's the design's
    //    natural max-content width. For flex-wrap layouts (the common
    //    case), this is where all bands fit on their designed number
    //    of rows without further reflow. Running a single probe at
    //    kFallbackDesignWidth would always pick that fallback as the
    //    width because flex children with flex-grow stretch to fill;
    //    the binary search finds the actual content boundary.
    //
    //    Yoga's real layout algorithm is more reliable than
    //    root.intrinsic_height() (which returns 0 for flex:row and
    //    display:grid containers) because it accounts for grid track
    //    sizing, absolute-positioned children, and nested column/row
    //    mixes. The probes are cheap (yoga only, no paint) and run
    //    once at startup before the window is created.
    auto probe_child_height = [&root](float probe_w) -> float {
        root.set_bounds({0.0f, 0.0f, probe_w, 99999.0f});
        root.layout_children();
        float h = 0.0f;
        for (size_t i = 0; i < root.child_count(); ++i) {
            const auto* c = root.child_at(i);
            if (!c || !c->visible()) continue;
            float ch = c->local_bounds().height;
            if (ch > h) h = ch;
        }
        return h;
    };
    if (design_w <= 0.0f && root.child_count() > 0) {
        constexpr float kWideProbe = 4000.0f;
        constexpr float kNarrowProbe = 200.0f;
        constexpr float kSettlingTolerance = 4.0f;  // px slack vs h_wide
        constexpr float kBinarySearchEpsilon = 16.0f;

        const float h_wide = probe_child_height(kWideProbe);
        const float h_narrow = probe_child_height(kNarrowProbe);

        // If narrow == wide, the design is non-responsive (no
        // flex-wrap / no reflow); just use the wide probe's settled
        // width by measuring the rightmost child edge instead.
        float settled_w = kFallbackDesignWidth;
        is_responsive = (h_narrow > h_wide + kSettlingTolerance);
        if (!is_responsive) {
            // Non-responsive: probe at fallback width and use child's
            // declared/measured width (or fallback).
            root.set_bounds({0.0f, 0.0f, kFallbackDesignWidth, 99999.0f});
            root.layout_children();
            float cw = 0.0f;
            for (size_t i = 0; i < root.child_count(); ++i) {
                const auto* c = root.child_at(i);
                if (!c || !c->visible()) continue;
                if (c->local_bounds().width > cw) cw = c->local_bounds().width;
            }
            settled_w = (cw >= kMinSensibleDim) ? cw : kFallbackDesignWidth;
        } else {
            // Responsive: binary-search for smallest width where
            // height stays ~ h_wide (layout has fully settled, no
            // more reflow rows).
            float lo = kNarrowProbe, hi = kWideProbe;
            while (hi - lo > kBinarySearchEpsilon) {
                const float mid = (lo + hi) * 0.5f;
                const float h_mid = probe_child_height(mid);
                if (h_mid > h_wide + kSettlingTolerance) lo = mid;
                else                                     hi = mid;
            }
            settled_w = hi;
        }

        // Re-run the final layout at settled_w to read the matching
        // height (binary search may have left root in an intermediate
        // state).
        const float settled_h = probe_child_height(settled_w);

        if (std::getenv("PULP_DESIGN_VIEWPORT_DEBUG")) {
            std::cerr << "[design-viewport] probe-scan: h_wide=" << h_wide
                      << " h_narrow=" << h_narrow
                      << " settled=" << settled_w << "x" << settled_h << "\n";
        }

        if (in_range(settled_w) && in_range(settled_h)) {
            design_w = settled_w;
            design_h = settled_h;
            viewport_source = "probe-layout (yoga-measured settling width)";
        } else if (std::getenv("PULP_DESIGN_VIEWPORT_DEBUG")) {
            std::cerr << "[design-viewport] probe-layout rejected: w="
                      << settled_w << " h=" << settled_h
                      << " (need both in [" << kMinSensibleDim
                      << ", " << kMaxSensibleDim << "])\n";
        }
    }

    // 4b. Fall back to intrinsic-only measurement if probe-layout
    //     didn't yield sensible values (e.g. tree built but every
    //     child invisible).
    if (design_w <= 0.0f) {
        float ih = root.intrinsic_height();
        float iw = root.intrinsic_width();
        for (size_t i = 0; i < root.child_count(); ++i) {
            const auto* c = root.child_at(i);
            if (!c || !c->visible()) continue;
            float cw = c->flex().preferred_width;
            if (cw > iw) iw = cw;
        }
        if (iw < kMinSensibleDim) iw = kFallbackDesignWidth;
        if (in_range(iw) && in_range(ih)) {
            design_w = iw;
            design_h = ih;
            viewport_source = "auto-measure (intrinsic fallback)";
        }
    }

    // 5. Fallback (Spectr-era default).
    if (design_w <= 0.0f) {
        design_w = kFallbackDesignWidth;
        design_h = kFallbackDesignHeight;
        viewport_source = "default 1320x860";
    }

    const float kDesignWidth  = design_w;
    const float kDesignHeight = design_h;
    std::cout << "Design viewport: " << kDesignWidth << " x " << kDesignHeight
              << " (source: " << viewport_source << ")\n";

    opts.width  = static_cast<uint32_t>(kDesignWidth);
    opts.height = static_cast<uint32_t>(kDesignHeight);
    // pulp-internal #70 — min size = settled size. The probe-scan
    // settled width is the smallest window where the JSX renders
    // without column-overlap. Going below that (which the previous
    // 0.5x min-floor allowed) shrunk the viewport's uniform scale
    // below 1.0 and produced unusably-tiny knob sizes, AND on some
    // drag paths macOS's aspect-lock would slip and the bands could
    // overlap. Pinning min to settled size + windowWillResize aspect-
    // snap together guarantee the window can only grow proportionally
    // above the natural authoring size, never shrink into the bad
    // zone.
    opts.min_width  = static_cast<uint32_t>(kDesignWidth);
    opts.min_height = static_cast<uint32_t>(kDesignHeight);
    opts.resizable = true;
    opts.use_gpu = true;
    opts.initially_hidden = no_show_window;  // pulp-internal #71

    // pulp #1899 — clamp any oversize absolute-positioned descendants
    // to the design viewport before first layout, so runtime-imported
    // trees with hardcoded oversize roots don't lay their bottom-
    // anchored children off-screen.
    pulp::view::reconcile_oversize_absolute_subtree(
        root,
        static_cast<uint32_t>(kDesignWidth),
        static_cast<uint32_t>(kDesignHeight));

    // pulp #2128 follow-up — bridge auto-registers in
    // WidgetBridge::all_bridges_ via its ctor, and the platform key
    // paths (window_host_mac.mm keyDown:/performKeyEquivalent:) call
    // WidgetBridge::dispatch_global_key, so any registerShortcut
    // callback or window.addEventListener('keydown', ...) listener in
    // the loaded ui.js fires without an explicit on_global_key lambda.

    auto window = WindowHost::create(root, opts);
    // Pin root at the probe-measured natural size and let paint
    // scale + letterbox to fit. Aspect-lock makes macOS enforce the
    // aspect on user drag — content always scales proportionally as
    // the user resizes, never reflows.
    window->set_design_viewport(kDesignWidth, kDesignHeight);
    window->set_fixed_aspect_ratio(kDesignWidth / kDesignHeight);
    bridge->set_repaint_callback([&window] {
        if (window) window->repaint();
    });
    window->set_close_callback([] {
        std::cout << "Window closed\n";
    });

    // No resize callback is needed: paint_scene reads design_viewport_w_/h_
    // every frame and re-pins root bounds at design size, so the window
    // can change size all it wants — the design surface stays put.

    // Hot reload — directory-watch mode so edits to ANY design-tool-*.js
    // module (or oklch.js) trigger a rebuild. The reload callback re-reads
    // the whole ordered module set rather than the single changed file's
    // text, since the split modules only form a valid program in sequence.
    HotReloader reloader(js_dir, js_path.filename().string(),
                         [&](const std::string& /*changed_code*/) {
        std::cout << "Hot reload: design-tool modules\n";
        try {
            // Validate the new scripts in an isolated bridge first so a bad
            // reload does not tear down the live UI.
            View probe_root;
            probe_root.set_theme(root.theme());
            probe_root.flex().direction = FlexDirection::column;
            StateStore probe_store;
            auto probe_engine = make_engine("reload:");
            auto probe_bridge = std::make_unique<WidgetBridge>(*probe_engine, probe_root, probe_store);
            probe_bridge->install_runtime_import_handlers();
            load_design_tool(*probe_bridge);

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
            load_design_tool(*next_bridge);
            next_bridge->restore_values(saved);
            engine = std::move(next_engine);
            bridge = std::move(next_bridge);
            bridge->set_repaint_callback([&window] {
                if (window) window->repaint();
            });
            // Codex P2 — re-run viewport reconciliation against the
            // freshly-rebuilt tree using the CURRENT window size, not
            // the initial opts.* values (the user may have resized
            // since launch). Without this, a hot-reloaded script with
            // oversize absolute roots regresses to the same off-screen
            // layout that the cold-start reconcile call (line ~295)
            // exists to prevent.
            if (window) {
                const auto sz = window->get_content_size();
                if (sz.width > 0 && sz.height > 0) {
                    pulp::view::reconcile_oversize_absolute_subtree(root, sz.width, sz.height);
                }
            }
            window->repaint();
        } catch (const std::exception& e) {
            std::cerr << "Hot reload failed: " << e.what() << "\n";
        } catch (...) {
            std::cerr << "Hot reload failed: unknown exception\n";
        }
    });

    // Poll hot-reload and async bridge results on a GCD timer (main thread).
    // poll_async_results() drains BOTH the async_exec_results_ queue
    // (setTimeout / IO / async-fn callbacks — required for React state
    // commits to land during drag) AND the pending_frame_ids queue
    // (RAF). CVDisplayLink alone doesn't drain the async-results
    // queue, so removing this call breaks drag-driven setState (drawing
    // filter bands silently stops mid-drag).
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
                // pulp-internal #71 — full per-tick bridge pump. Both halves
                // are required: poll_async_results drains async-exec
                // results + queued frame callbacks; service_frame_callbacks
                // drains JS setTimeout / setInterval timers via
                // __flushTimers__. Skipping the second call leaves every
                // imported app's polling-state-update path frozen — the
                // app's React tree never re-evaluates because setInterval
                // callbacks queue forever (see scripted_ui.cpp:67-81 +
                // tools/scripts/host_pump_lint.py).
                (*bridge_slot)->poll_async_results();
                (*bridge_slot)->service_frame_callbacks();
            }
            reloader_ptr->poll_reload();
        } catch (const std::exception& e) {
            std::cerr << "Design tool event loop error: " << e.what() << "\n";
        } catch (...) {
            std::cerr << "Design tool event loop error: unknown exception\n";
        }
    });
    dispatch_resume(reload_timer);

    // pulp-internal #71 — auto-exit support for live-host smoke tests
    // (tools/import-validation/live-host-pump-smoke.sh). dispatch_after on
    // the main queue calls window->request_close() cleanly so the run loop
    // exits via the normal close path rather than SIGTERM. No-op when
    // exit_after_ms == 0 (the default for interactive use).
    if (exit_after_ms > 0) {
        auto* window_ptr = window.get();
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                                      static_cast<int64_t>(exit_after_ms) * NSEC_PER_MSEC),
                       dispatch_get_main_queue(), ^{
            if (window_ptr) window_ptr->request_close();
        });
    }

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
