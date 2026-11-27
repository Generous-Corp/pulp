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

#elif defined(PULP_VELLUM_UI_COMPAT_PROBE_VIEW_AUDIO)
// The audio widgets package keeps its keyboard controller in a header, so this
// proves the include path and the Vellum input types it is built on; the
// package's compiled library is not part of this probe's link.
#include <pulp/vellum_audio/musical_typing.hpp>

int main() {
    pulp::vellum_audio::MusicalTypingController typing;
    int sounded = -1;
    typing.on_note_on = [&](int note, float) { sounded = note; };
    pulp::vellum_audio::KeyEvent press;
    press.key = pulp::vellum_audio::KeyCode::a;
    const bool consumed = typing.handle_key(press);
#if defined(PULP_VELLUM_UI_COMPAT_VIEW_ALONE)
    std::printf("Pulp::view alone -> MusicalTypingController 'a' sounds note %d\n", sounded);
#else
    std::printf("Pulp::view-audio -> MusicalTypingController 'a' sounds note %d\n", sounded);
#endif
    return consumed && sounded == typing.base_note() ? 0 : 1;
}

#elif defined(PULP_VELLUM_UI_COMPAT_PROBE_VIEW_TOOLING)
// The screenshot surface is defined in the provider library behind the name, so
// calling it makes the link depend on that library. EditorBridge comes from the
// view-free editor-bridge target the name links, and a WebView-editor plug-in
// dispatches through it.
#include <pulp/view/editor_bridge.hpp>
#include <vellum/view/screenshot.hpp>

#include <string>

int main() {
    const bool capable = vellum::view::has_screenshot_backend();
    pulp::view::EditorBridge bridge;
    bridge.add_handler("ping", [](const choc::value::ValueView&) {
        return pulp::view::EditorBridge::ok_response();
    });
    const std::string reply = bridge.dispatch_json(R"({"type":"ping"})");
    const bool dispatched = reply == pulp::view::EditorBridge::ok_response();
    std::printf("Pulp::view-tooling -> vellum::view::has_screenshot_backend %s, EditorBridge %s\n",
                capable ? "yes" : "no", dispatched ? "dispatched" : "failed");
    return dispatched ? 0 : 1;
}

#else
#error "define one PULP_VELLUM_UI_COMPAT_PROBE_<NAME>"
#endif
