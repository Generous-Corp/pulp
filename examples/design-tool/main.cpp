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
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_map>

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

int main(int argc, char* argv[]) {
    // Find the JS file
    fs::path js_path;
    if (argc > 1) {
        js_path = argv[1];
    } else {
        auto dir = fs::current_path();
        while (!dir.empty()) {
            auto candidate = dir / "examples" / "design-tool" / "design-tool.js";
            if (fs::exists(candidate)) { js_path = candidate; break; }
            auto parent = dir.parent_path();
            if (parent == dir) break;
            dir = parent;
        }
    }

    if (js_path.empty() || !fs::exists(js_path)) {
        std::cerr << "Error: design-tool.js not found.\n";
        std::cerr << "Usage: pulp-design-tool [path/to/design-tool.js]\n";
        return 1;
    }

    std::cout << "Loading: " << js_path.string() << "\n";

    // Create the view tree root
    View root;
    root.set_theme(Theme::dark());
    root.flex().direction = FlexDirection::row;

    StateStore store;
    ScriptEngine engine;
    engine.set_log_callback([](std::string_view level, std::string_view msg) {
        std::cerr << "[" << level << "] " << msg << "\n";
    });
    WidgetBridge bridge(engine, root, store);

    // Load library scripts first (oklch.js)
    auto js_dir = js_path.parent_path();
    for (auto& lib : {"oklch.js"}) {
        auto lib_path = js_dir / lib;
        if (fs::exists(lib_path)) {
            bridge.load_script(read_file(lib_path));
            std::cout << "Loaded: " << lib << "\n";
        }
    }

    // Load main UI script
    auto code = read_file(js_path);
    if (code.empty()) {
        std::cerr << "Error: could not read " << js_path.string() << "\n";
        return 1;
    }
    bridge.load_script(code);

    std::cout << "UI created: " << root.child_count() << " top-level views\n";

    // Open window
    WindowOptions opts;
    opts.title = "Pulp Style Designer";
    opts.width = 1100;
    opts.height = 700;
    opts.min_width = 1000;  // Issue 5: prevent scrollbar overlap
    opts.min_height = 600;
    opts.resizable = true;

    auto window = WindowHost::create(root, opts);
    window->set_close_callback([] {
        std::cout << "Window closed\n";
    });

    // Hot reload
    HotReloader reloader(js_path, [&](const std::string& new_code) {
        std::cout << "Hot reload: " << js_path.filename().string() << "\n";
        std::unordered_map<std::string, float> saved;
        bridge.snapshot_values(saved);
        bridge.clear();
        bridge.load_script(new_code);
        bridge.restore_values(saved);
        window->repaint();
    });

    std::cout << "Hot reload watching: " << js_path.parent_path().string() << "\n";
    std::cout << "Opening window...\n";

    window->run_event_loop();
    return 0;
}
