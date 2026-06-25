// Ink & Signal Sampler — a starter sampler UI assembled entirely from the
// pulp::design component catalog. It proves the design system is usable
// end-to-end: knobs, waveform, stepper, pan, meter, badge, and buttons,
// all token-driven and reskinnable.
//
//   pulp-ink-signal-sampler --out /tmp/sampler [--theme light|dark|both]
//
// Default writes <out>-light.png and <out>-dark.png, headless.

#include <pulp/design/sampler_starter.hpp>
#include <pulp/design/design_system.hpp>
#include <pulp/view/screenshot.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

using namespace pulp::design;
using namespace pulp::view;

int main(int argc, char** argv) {
    std::string out = "ink-signal-sampler";
    std::string theme = "both";
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!std::strcmp(argv[i], "--theme") && i + 1 < argc) theme = argv[++i];
    }

    auto render = [&](bool dark) -> bool {
        auto root = build_sampler_starter(ink_signal_theme(dark));
        const uint32_t W = static_cast<uint32_t>(kSamplerWidth);
        const uint32_t H = static_cast<uint32_t>(kSamplerHeight);
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
