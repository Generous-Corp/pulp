// pulp-screenshot — Render a plugin UI to PNG for visual validation
// Used by: pulp CLI (`pulp screenshot`), MCP server, CI pipelines

#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/state/store.hpp>
#include <iostream>
#include <fstream>
#include <string>

using namespace pulp::view;
using namespace pulp::state;

static void print_usage() {
    std::cerr << "Usage: pulp-screenshot [options]\n";
    std::cerr << "  --script <file.js>   JS UI script to render\n";
    std::cerr << "  --output <file.png>  Output PNG path (default: screenshot.png)\n";
    std::cerr << "  --width <px>         Width in points (default: 400)\n";
    std::cerr << "  --height <px>        Height in points (default: 300)\n";
    std::cerr << "  --scale <factor>     Scale factor (default: 2.0)\n";
    std::cerr << "  --theme <name>       Theme: dark, light, pro_audio (default: dark)\n";
    std::cerr << "  --base64             Output base64-encoded PNG to stdout\n";
    std::cerr << "  --demo               Render a demo UI (no script needed)\n";
}

static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return {};
    return std::string(std::istreambuf_iterator<char>(f), {});
}

static std::string base64_encode(const std::vector<uint8_t>& data) {
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);

    for (size_t i = 0; i < data.size(); i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < data.size()) n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < data.size()) n |= static_cast<uint32_t>(data[i + 2]);

        out += table[(n >> 18) & 0x3F];
        out += table[(n >> 12) & 0x3F];
        out += (i + 1 < data.size()) ? table[(n >> 6) & 0x3F] : '=';
        out += (i + 2 < data.size()) ? table[n & 0x3F] : '=';
    }
    return out;
}

int main(int argc, char* argv[]) {
    std::string script_path;
    std::string output_path = "screenshot.png";
    uint32_t width = 400, height = 300;
    float scale = 2.0f;
    std::string theme_name = "dark";
    bool output_base64 = false;
    bool demo = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--script" && i + 1 < argc) script_path = argv[++i];
        else if (arg == "--output" && i + 1 < argc) output_path = argv[++i];
        else if (arg == "--width" && i + 1 < argc) width = std::stoi(argv[++i]);
        else if (arg == "--height" && i + 1 < argc) height = std::stoi(argv[++i]);
        else if (arg == "--scale" && i + 1 < argc) scale = std::stof(argv[++i]);
        else if (arg == "--theme" && i + 1 < argc) theme_name = argv[++i];
        else if (arg == "--base64") output_base64 = true;
        else if (arg == "--demo") demo = true;
        else if (arg == "--help" || arg == "-h") { print_usage(); return 0; }
    }

    if (!demo && script_path.empty()) {
        std::cerr << "Error: --script or --demo required\n";
        print_usage();
        return 1;
    }

    // Set up state store
    StateStore store;

    // Create root view with theme
    View root;
    if (theme_name == "light") root.set_theme(Theme::light());
    else if (theme_name == "pro_audio") root.set_theme(Theme::pro_audio());
    else root.set_theme(Theme::dark());

    // pulp #1899 — apply --width/--height to the root's bounds BEFORE
    // any script runs. Without this, root.local_bounds() is (0,0,0,0)
    // when yoga_layout reads it; YGNodeStyleSetWidth/Height(root) then
    // gets 0, and every position:absolute + inset:0 child computes to
    // 0×0 — blanking any chain of absolute-positioned containers (the
    // canonical "fill containing block" CSS pattern). First surfaced
    // via Spectr's editor.generated.tsx: Editor / FilterBank / canvas /
    // Chrome hierarchy is exactly this chain.
    root.set_bounds({0, 0, static_cast<float>(width), static_cast<float>(height)});

    root.flex().direction = FlexDirection::column;
    // Only set padding/gap for demo mode — scripts manage their own layout
    if (demo) {
        root.flex().padding = 16;
        root.flex().gap = 8;
    }

    // Set up scripting
    ScriptEngine engine;
    WidgetBridge bridge(engine, root, store);

    // pulp #1899 — install runtime-import handlers so React-imported
    // trees (Spectr's editor.js et al.) can register & drain useEffect /
    // requestAnimationFrame / setTimeout callbacks. These are registered
    // conditionally because plain createKnob / createFader scripts don't
    // need them. Always install in pulp-screenshot since the tool's
    // raison d'etre includes capturing React-driven imports.
    bridge.install_runtime_import_handlers();

    if (demo) {
        // Built-in demo UI
        bridge.load_script(R"(
            createLabel('title', 'Pulp Plugin Demo', 0, 0, 300, 24);
            createKnob('gain', 0, 0, 64, 64);
            createKnob('mix', 0, 0, 64, 64);
            createFader('volume', 0, 0, 200, 20, 'horizontal');
            createToggle('bypass', 0, 0, 60, 28);
        )");
        engine.evaluate("setValue('gain', 0.6)");
        engine.evaluate("setValue('mix', 0.8)");
        engine.evaluate("setValue('volume', 0.7)");
    } else {
        // Load library JS files from the same directory
        auto js_dir = std::filesystem::path(script_path).parent_path();
        for (auto& lib : {"oklch.js"}) {
            auto lib_path = js_dir / lib;
            if (std::filesystem::exists(lib_path)) {
                bridge.load_script(read_file(lib_path.string()));
            }
        }

        auto code = read_file(script_path);
        if (code.empty()) {
            std::cerr << "Error: could not read " << script_path << "\n";
            return 1;
        }
        bridge.load_script(code);
    }

    // pulp #1899 — drain React's useEffect callbacks, requestAnimationFrame
    // queue, and setTimeout/setInterval timers BEFORE rendering. Without
    // this, headless captures of React-imported trees show only what
    // mounts synchronously — any drawing that lives inside useEffect
    // (canvas paint, dB axis labels, frequency labels, grid lines) never
    // runs because the underlying message loop doesn't tick between
    // script-load and render. Spectr's editor uses this pattern for
    // drawSpectrum / drawRulers; the live host's NSRunLoop ticks them
    // naturally, but pulp-screenshot's headless path has to pump
    // explicitly. __pulpRuntimeSettle__ is registered by WidgetBridge
    // exactly for this case (see widget_bridge.cpp:1144).
    bridge.load_script("if (typeof __pulpRuntimeSettle__ === 'function') __pulpRuntimeSettle__(64);");

    // Render
    if (output_base64) {
        auto png = render_to_png(root, width, height, scale);
        if (png.empty()) {
            std::cerr << "Error: rendering failed\n";
            return 1;
        }
        std::cout << base64_encode(png);
        return 0;
    } else {
        bool ok = render_to_file(root, width, height, output_path, scale);
        if (!ok) {
            std::cerr << "Error: rendering failed\n";
            return 1;
        }
        std::cout << "Screenshot saved to " << output_path << " (" << width << "x" << height << " @" << scale << "x)\n";
        return 0;
    }
}
