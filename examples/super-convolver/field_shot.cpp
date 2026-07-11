// Native render of the SuperConvolver "acoustic field" using Pulp's real canvas
// (Skia raster capture) — a still frame of the shared field renderer that also
// drives the live plugin editor. Headless, no window, no audio.
//
//   super-convolver-field-shot [out.png]

#include "field_renderer.hpp"

#include <pulp/canvas/canvas.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/view.hpp>

#include <cstdio>

namespace {

class FieldView : public pulp::view::View {
public:
    void paint(pulp::canvas::Canvas& c) override {
        const auto b = local_bounds();
        c.set_fill_color(pulp::canvas::Color::rgba8(5, 6, 8));
        c.fill_rect(0, 0, b.width, b.height);
        // A frozen, flowing frame at moderate motion and density.
        pulp::superconvolver::draw_acoustic_field(
            c, 0, 0, b.width, b.height, /*t=*/6.0, /*flow=*/0.62f,
            /*density=*/72, /*energy=*/0.6f);
    }
};

}  // namespace

int main(int argc, char** argv) {
    const char* out = (argc > 1) ? argv[1] : "/tmp/super_convolver_field.png";
    FieldView view;
    view.set_bounds({0, 0, 960, 600});
    const bool ok = pulp::view::render_to_file(view, 960, 600, out, 2.0f,
                                               pulp::view::ScreenshotBackend::skia);
    std::printf("SuperConvolver field: %s -> %s\n", ok ? "OK" : "FAILED", out);
    return ok ? 0 : 1;
}
