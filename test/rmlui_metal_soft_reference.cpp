// rmlui_metal_soft_reference.cpp — writes the reference frames the Metal
// renderer golden test compares against: each document rendered by Vellum's
// SoftRenderer at each frame size, as premultiplied RGBA8 files.
//
//   pulp-rmlui-soft-reference <output-dir>
//
// Runs in its own process (Vellum's Core module carries its own RmlUi); ctest
// runs it as the fixture the golden test requires.

#include "rmlui_metal_golden.hpp"

#include <vellum/rmlui/backend.hpp>
#include <vellum/rmlui/soft_renderer.hpp>

#include <cstdio>

namespace {

using namespace vellum::rmlui;

// The documents run no scripts.
class NoLua final : public LuaInterface {
  public:
    Status load(std::string_view, std::string_view) override {
        return Status::ok;
    }
    Status dispatch(std::string_view) override {
        return Status::ok;
    }
};

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <output-dir>\n", argv[0]);
        return 2;
    }
    if (!Backend::fonts_enabled()) {
        std::fprintf(stderr, "the references need a Vellum built with VELLUM_RMLUI_FONTS=ON\n");
        return 2;
    }
    const std::filesystem::path out_dir(argv[1]);
    std::filesystem::create_directories(out_dir);
    pulp_rmlui_golden::GoldenAssets assets(PULP_RMLUI_GOLDEN_FONT);
    int written = 0;
    for (const auto& document : pulp_rmlui_golden::kDocuments) {
        for (const auto scale : pulp_rmlui_golden::kScales) {
            const std::uint32_t width = pulp_rmlui_golden::kWidthPoints * scale;
            const std::uint32_t height = pulp_rmlui_golden::kHeightPoints * scale;
            SoftRenderer renderer(0x00000000u); // transparent, as a cleared Metal frame
            NoLua lua;
            Backend backend(assets, renderer, lua);
            if (backend.open(width, height) != Status::ok) {
                std::fprintf(stderr, "open failed at %ux%u\n", width, height);
                return 1;
            }
            FontFaceOptions face;
            face.style = FontStyle::normal;
            face.weight = 400;
            face.fallback = true;
            if (backend.load_font_face(pulp_rmlui_golden::kFontAsset, "Inter", face) !=
                Status::ok) {
                std::fprintf(stderr, "font failed: %s\n", PULP_RMLUI_GOLDEN_FONT);
                return 1;
            }
            if (backend.load_document(document.name) != Status::ok) {
                std::fprintf(stderr, "document failed: %s\n", document.name);
                return 1;
            }
            backend.render();
            backend.render(); // a second pass, after layout settles
            if (renderer.skipped_draws() != 0) {
                std::fprintf(stderr, "%s: the soft renderer skipped %zu draws\n", document.name,
                             static_cast<std::size_t>(renderer.skipped_draws()));
                return 1;
            }
            const auto file =
                out_dir / pulp_rmlui_golden::reference_name(document.name, width, height);
            if (!pulp_rmlui_golden::write_rgba(file, renderer.pixels())) {
                std::fprintf(stderr, "could not write %s\n", file.string().c_str());
                return 1;
            }
            backend.close();
            ++written;
        }
    }
    std::printf("wrote %d reference frame(s) to %s\n", written, out_dir.string().c_str());
    return 0;
}
