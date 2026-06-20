// Widget Gallery — renders the Ink & Signal component gallery (every supported
// primitive in one board) to PNG, headless. It triples as living documentation,
// a reskin-preview surface, and the visual-regression corpus used to compare
// theme/rendering changes. Swap --preset / --theme to preview a reskin.
//
//   pulp-widget-gallery --out /tmp/gallery [--theme light|dark|both] [--preset ink-signal]
//
// Default writes <out>-light.png and <out>-dark.png.

#include <pulp/view/widget_gallery.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/theme_presets.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

using namespace pulp::view;

int main(int argc, char** argv) {
    std::string out = "widget-gallery";
    std::string theme = "both";
    std::string preset = "ink-signal";
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!std::strcmp(argv[i], "--theme") && i + 1 < argc) theme = argv[++i];
        else if (!std::strcmp(argv[i], "--preset") && i + 1 < argc) preset = argv[++i];
    }

    const ThemePreset* p = find_preset(preset);
    if (!p) {
        std::fprintf(stderr, "widget-gallery: unknown preset '%s'\n", preset.c_str());
        return 1;
    }

    auto render = [&](bool dark) -> bool {
        auto root = build_widget_gallery(theme_from_preset(*p, dark));
        const uint32_t W = static_cast<uint32_t>(GALLERY_WIDTH);
        const uint32_t H = static_cast<uint32_t>(root->bounds().height);
        const std::string path = out + (dark ? "-dark.png" : "-light.png");
        const bool ok = render_to_file(*root, W, H, path, 2.0f,
                                       ScreenshotBackend::default_backend);
        std::printf("%s %s (%ux%u)\n", ok ? "wrote" : "FAILED", path.c_str(), W, H);
        return ok;
    };

    bool ok = true;
    if (theme != "dark") ok &= render(false);
    if (theme != "light") ok &= render(true);
    return ok ? 0 : 1;
}
