// One probe per Vellum-backed Pulp UI name. Each build defines exactly one
// PULP_VELLUM_UI_COMPAT_PROBE_<NAME>, includes the Vellum header that name must
// expose, and calls into the library behind it so the link is not vacuous.

#include <cstdio>
#include <memory>

#if defined(PULP_VELLUM_UI_COMPAT_PROBE_CANVAS)
#include <vellum/canvas/canvas.hpp>

int main() {
    const auto color = vellum::canvas::Color::from_hsv({0.0f, 1.0f, 1.0f});
    std::printf("Pulp::canvas -> vellum::canvas::Color r=%.2f\n", color.r);
    return color.r > 0.99f ? 0 : 1;
}

#elif defined(PULP_VELLUM_UI_COMPAT_PROBE_RENDER)
// render/gpu_surface.hpp is outside the installed mirror's supported surface
// (it raises #error), so the probe uses a supported render header whose
// implementation lives in the library.
#include <vellum/render/texture_atlas.hpp>

int main() {
    vellum::render::AtlasPacker packer(64, 32);
    vellum::render::AtlasPacker::Region region;
    const bool ok = packer.allocate(16, 8, region) && region.w == 16 && region.h == 8;
    std::printf("Pulp::render -> vellum::render::AtlasPacker::allocate %s\n", ok ? "ok" : "failed");
    return ok ? 0 : 1;
}

#elif defined(PULP_VELLUM_UI_COMPAT_PROBE_VIEW) ||                                                 \
    defined(PULP_VELLUM_UI_COMPAT_PROBE_DESIGN_IMPORT)
#include <vellum/view/design_import.hpp>
#if defined(PULP_VELLUM_UI_COMPAT_PROBE_VIEW)
#include <vellum/view/plugin_view_host.hpp>
#include <vellum/view/view.hpp>
#define PULP_VELLUM_UI_COMPAT_NAME "Pulp::view"
#else
#define PULP_VELLUM_UI_COMPAT_NAME "Pulp::design-import"
#endif

int main() {
    const auto ir = vellum::view::parse_designmd_yaml("---\n"
                                                      "name: Installed SDK probe\n"
                                                      "colors:\n"
                                                      "  primary: \"#336699\"\n"
                                                      "---\n");
    const auto primary = ir.tokens.colors.find("primary");
    const bool ok = primary != ir.tokens.colors.end() && primary->second == "#336699";
    std::printf("%s -> vellum::view::parse_designmd_yaml primary=%s\n", PULP_VELLUM_UI_COMPAT_NAME,
                ok ? primary->second.c_str() : "missing");
    return ok ? 0 : 1;
}

#else
#error "define one PULP_VELLUM_UI_COMPAT_PROBE_<NAME>"
#endif
