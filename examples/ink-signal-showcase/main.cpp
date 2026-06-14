// Ink & Signal Showcase — a live, GPU-backed window rendering every
// design-system primitive so you can see how they render and feel them
// (hover a knob for the glow, drag faders/sliders, click toggles, switch
// tabs). Built from build_widget_gallery() so it stays in lockstep with the
// gallery/component goldens.
//
//   pulp-ink-signal-showcase                       # live GPU window (dark)
//   pulp-ink-signal-showcase --theme light         # live GPU window (light)
//   pulp-ink-signal-showcase --screenshot out.png  # headless GPU/Skia render
//
// GPU requires a Skia-enabled build (configure with -DSKIA_DIR=<skia-build>);
// without it the window host falls back to the CPU raster path.

#include <pulp/view/widget_gallery.hpp>
#include <pulp/view/theme_presets.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/window_host.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/ui_components.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

using namespace pulp::view;

namespace {

// Advance the animated widgets each frame so hover glow / toggle slide / scroll
// easing actually move in the live window (mirrors ui-preview).
void advance_widget_animations(View* v, float dt) {
    if (!v) return;
    if (auto* k = dynamic_cast<Knob*>(v)) k->advance_animations(dt);
    else if (auto* t = dynamic_cast<Toggle*>(v)) t->advance_animations(dt);
    else if (auto* f = dynamic_cast<Fader*>(v)) f->advance_animations(dt);
    else if (auto* s = dynamic_cast<ScrollView*>(v)) s->advance_animations(dt);
    for (std::size_t i = 0; i < v->child_count(); ++i)
        advance_widget_animations(v->child_at(i), dt);
}

}  // namespace

int main(int argc, char** argv) {
    std::string theme_name = "dark";
    std::string preset = "ink-signal";
    std::string screenshot;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--theme") && i + 1 < argc) theme_name = argv[++i];
        else if (!std::strcmp(argv[i], "--preset") && i + 1 < argc) preset = argv[++i];
        else if (!std::strcmp(argv[i], "--screenshot") && i + 1 < argc) screenshot = argv[++i];
    }

    const ThemePreset* p = find_preset(preset);
    if (!p) { std::fprintf(stderr, "unknown preset '%s'\n", preset.c_str()); return 1; }
    const bool dark = theme_name != "light";

    auto root = build_widget_gallery(theme_from_preset(*p, dark));
    const uint32_t W = static_cast<uint32_t>(GALLERY_WIDTH);
    const uint32_t H = static_cast<uint32_t>(root->bounds().height);

    // Headless GPU/Skia render — proves the GPU pipeline without a window.
    if (!screenshot.empty()) {
        auto png = render_to_png(*root, W, H, 2.0f, ScreenshotBackend::skia);
        if (png.empty()) {
            std::fprintf(stderr, "render produced no PNG (no Skia/GPU backend?)\n");
            return 1;
        }
        std::ofstream out(screenshot, std::ios::binary);
        out.write(reinterpret_cast<const char*>(png.data()),
                  static_cast<std::streamsize>(png.size()));
        std::printf("wrote %s (%ux%u, %zu bytes, Skia backend)\n",
                    screenshot.c_str(), W, H, png.size());
        return 0;
    }

    // Live GPU window.
    FrameClock clock;
    root->set_frame_clock(&clock);

    WindowOptions opts;
    opts.title = "Ink & Signal — Showcase";
    opts.width = static_cast<int>(W);
    opts.height = static_cast<int>(H);
    opts.use_gpu = true;

    auto window = WindowHost::create(*root, opts);
    if (!window) { std::fprintf(stderr, "failed to create window host\n"); return 1; }

    View* root_ptr = root.get();
    window->set_idle_callback([root_ptr]() {
        advance_widget_animations(root_ptr, 1.0f / 60.0f);
        root_ptr->request_repaint();
    });
    window->set_close_callback([]() {});

    std::printf("Ink & Signal showcase — %s, %ux%u (GPU window). Close to exit.\n",
                dark ? "dark" : "light", W, H);
    window->run_event_loop();
    return 0;
}
