// svg_probe — render an SVG document through Pulp's DesignFrameView (SkSVGDOM)
// to a PNG, so we can confirm the faithful-vector render path reproduces a
// Figma-exported SVG 1:1. Diagnostic tool for the Figma→Pulp import lane.
//
//   svg_probe <in.svg> <out.png> [width] [height]
#include <pulp/view/design_frame_view.hpp>
#include <pulp/view/screenshot.hpp>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: svg_probe <in.svg> <out.png> [w] [h]\n"); return 2; }
    std::ifstream f(argv[1], std::ios::binary);
    if (!f) { std::fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    std::string svg((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    const uint32_t W = argc > 3 ? static_cast<uint32_t>(std::atoi(argv[3])) : 1356;
    const uint32_t H = argc > 4 ? static_cast<uint32_t>(std::atoi(argv[4])) : 781;

    pulp::view::DesignFrameView view(std::move(svg), {});
    view.set_bounds({0.0f, 0.0f, static_cast<float>(W), static_cast<float>(H)});

    auto png = pulp::view::render_to_png(view, W, H, 2.0f, pulp::view::ScreenshotBackend::skia);
    std::ofstream out(argv[2], std::ios::binary);
    out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    std::fprintf(stderr, "wrote %s (%ux%u, %zu bytes)\n", argv[2], W, H, png.size());
    return 0;
}
