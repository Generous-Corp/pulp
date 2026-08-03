// Renders THIS plugin's editor headlessly — no window, no audio device.
//
// The subject is `Processor::create_view()`, the exact call every format
// adapter makes to obtain its editor. Rendering the importer's own tree instead
// would prove the importer works and say nothing about what the plugin shows,
// which is the substitution this tool exists to refuse.

#include "design_panel_instrument.hpp"

#include <pulp/view/screenshot.hpp>

#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    std::string out = "editor.png";
    float width = 900.0f;
    float height = 602.0f;
    float scale = 2.0f;
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

    auto processor = pulp::examples::create_design_panel();
    auto editor = processor->create_view();
    if (editor == nullptr) {
        std::cerr << "create_view() returned no editor — the plugin carries no "
                     "usable design\n";
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
    std::cout << "editor rendered to " << out << "\n";
    return 0;
}
