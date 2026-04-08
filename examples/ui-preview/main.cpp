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
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <filesystem>

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

bool parse_point(const std::string& text, Point& point) {
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

Point center_in_root(const View& root, const View& target) {
    Point center{
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

}  // namespace

int main(int argc, char* argv[]) {
    bool screenshot_only = false;
    std::string screenshot_path = "/tmp/pulp-animation-preview.png";
    std::string view_tree_path;
    std::string script_path;
    int render_w = 360, render_h = 480;

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
    }

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

                    Point click_point{};
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
