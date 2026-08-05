// Renders the editor through ViewBridge — the layer CLAP, VST3, AU and AUv3
// all go through to obtain one.
//
// The distinction from render_editor.cpp is the point: that tool calls
// Processor::create_view() directly, which proves the plugin CAN draw the
// design. This one goes through the adapters' own editor-lifecycle path
// (construct the bridge over the processor and its StateStore, open it, take
// the root it built), so a regression that lands between the adapter and
// create_view() — a bridge that substitutes an auto-generated UI, a scripted
// ui.js that wins over the native tree — fails here and passes there.
//
// `open()` is deliberately not followed by `notify_attached()`: attachment is
// the host's native-window step, and the editor's CONTENT is fully built
// without it. That is what makes this runnable with no window and no host.

#include "design_panel_midi_effect.hpp"

#include <pulp/format/view_bridge.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/screenshot.hpp>

#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    std::string out = "adapter-editor.png";
    float width = 900.0f, height = 602.0f, scale = 2.0f;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string arg = argv[i];
        if (arg == "--output") out = argv[i + 1];
        else if (arg == "--width") width = std::strtof(argv[i + 1], nullptr);
        else if (arg == "--height") height = std::strtof(argv[i + 1], nullptr);
        else if (arg == "--scale") scale = std::strtof(argv[i + 1], nullptr);
        else {
            std::cerr << "usage: --output <png> [--width W] [--height H] "
                         "[--scale S]\n";
            return 2;
        }
    }

    auto processor = pulp::examples::create_design_panel_midi_effect();
    pulp::state::StateStore store;
    processor->define_parameters(store);

    pulp::format::ViewBridge bridge(*processor, store);
    std::string error;
    if (!bridge.open(&error)) {
        std::cerr << "ViewBridge::open() failed: " << error << "\n";
        return 1;
    }
    pulp::view::View* editor = bridge.view();
    if (editor == nullptr) {
        std::cerr << "ViewBridge opened with no view\n";
        return 1;
    }
    editor->set_bounds({0.0f, 0.0f, width, height});

    if (!pulp::view::render_to_file(
            *editor, static_cast<std::uint32_t>(width),
            static_cast<std::uint32_t>(height), out, scale,
            pulp::view::ScreenshotBackend::skia)) {
        std::cerr << "render failed\n";
        return 1;
    }
    std::cout << "adapter editor rendered to " << out << "\n";
    return 0;
}
