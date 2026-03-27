// UI Preview — validates the full rendering pipeline with animations
// JS → WidgetBridge → View tree → layout → paint → CoreGraphics/GPU → screen
// Hover over knobs to see glow, click toggle to see animated thumb slide

#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/window_host.hpp>
#include <pulp/state/store.hpp>
#include <iostream>
#include <cstring>

using namespace pulp::view;
using namespace pulp::state;

int main(int argc, char* argv[]) {
    bool screenshot_only = (argc > 1 && std::strcmp(argv[1], "--screenshot") == 0);
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

    // Build UI — use flex layout so widgets get real sizes
    // Title label
    auto title = std::make_unique<Label>("Pulp Animation Preview");
    title->set_font_size(16.0f);
    title->flex().preferred_height = 24;
    root.add_child(std::move(title));

    // Knobs row
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

    // Toggle
    auto bypass = std::make_unique<Toggle>();
    bypass->set_label("Bypass");
    bypass->flex().preferred_width = 60;
    bypass->flex().preferred_height = 28;
    root.add_child(std::move(bypass));

    // Horizontal fader — fixed height, not flex_grow (avoids huge hover area)
    auto fader = std::make_unique<Fader>();
    fader->set_label("Volume");
    fader->set_value(0.65f);
    fader->set_orientation(Fader::Orientation::horizontal);
    fader->flex().preferred_height = 32;
    root.add_child(std::move(fader));

    // Scrollable preset list — demonstrates smooth scroll + animated scroll bars
    auto scroll = std::make_unique<ScrollView>();
    scroll->flex().flex_grow = 1;
    scroll->set_content_size({0, 600}); // taller than visible area

    // Add labels as scrollable content
    for (int i = 0; i < 20; i++) {
        auto item = std::make_unique<Label>("Preset " + std::to_string(i + 1));
        item->set_bounds({8, static_cast<float>(i * 28 + 4), 300, 24});
        scroll->add_child(std::move(item));
    }
    root.add_child(std::move(scroll));

    std::cout << "UI Preview: " << root.child_count() << " widgets created\n";

    // Headless screenshot mode: render to PNG and exit
    if (screenshot_only) {
        root.set_bounds({0, 0, 360, 480});
        root.layout_children();
        bool ok = render_to_file(root, 360, 480, "/tmp/pulp-animation-preview.png");
        std::cout << (ok ? "Screenshot saved to /tmp/pulp-animation-preview.png\n"
                        : "Screenshot failed\n");
        return ok ? 0 : 1;
    }

    std::cout << "Hover over knobs to see glow animation\n";
    std::cout << "Click the toggle to see animated thumb slide\n";
    std::cout << "Hover over fader to see thumb scale\n";

    // Open window and run
    WindowOptions opts;
    opts.title = "Pulp Animation Preview";
    opts.width = 360;
    opts.height = 480;

    auto window = WindowHost::create(root, opts);
    window->set_close_callback([] {
        std::cout << "Window closed\n";
    });

    std::cout << "Opening window...\n";
    window->run_event_loop();

    return 0;
}
