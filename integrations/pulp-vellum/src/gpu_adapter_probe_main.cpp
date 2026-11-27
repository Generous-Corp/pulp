// pulp-vellum-gpu-adapter-probe: render a fixed scene on the installed Vellum
// GPU runtime, capture it, and judge the adapter identity and the pixels with
// the shared verdict rules. Prints one JSON line; exit 0 pass, 1 fail,
// 2 unavailable.
//
//   --simulate-empty-adapter   drop the adapter name before judging, which the
//                              rules must refuse (negative control)

#include <pulp_tooling/gpu_probe/adapter_probe.hpp>

#include <vellum/graphics/color.hpp>
#include <vellum/graphics/dawn_native_bootstrap.hpp>
#include <vellum/graphics/scene.hpp>
#include <vellum/graphics/skia_dawn_surface.hpp>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace probe = pulp::tooling::gpu_adapter;
namespace vg = vellum::graphics;

namespace {

vg::Scene probe_scene() {
    auto card = [](const char* id, float x, std::uint32_t color) {
        vg::SceneNode node;
        node.id = id;
        node.kind = vg::SceneNode::Kind::rectangle;
        node.bounds = {x, 10.0F, 40.0F, 30.0F};
        node.fill = vg::Color::hex(color);
        return node;
    };
    vg::Scene scene;
    scene.width = static_cast<float>(probe::kSceneWidth);
    scene.height = static_cast<float>(probe::kSceneHeight);
    scene.background = vg::Color::hex(0x0F172A);
    scene.root.id = "probe/root";
    scene.root.kind = vg::SceneNode::Kind::group;
    scene.root.bounds = {0.0F, 0.0F, scene.width, scene.height};
    scene.root.children = {card("probe/teal", 10.0F, 0x14B8A6),
                           card("probe/amber", 60.0F, 0xF59E0B)};
    return scene;
}

probe::Evidence to_evidence(const vg::GpuEvidence& source) {
    probe::Evidence e;
    e.available = source.available;
    e.native_surface = source.native_surface;
    e.fallback = source.fallback;
    e.renderer = source.renderer;
    e.backend = source.backend;
    e.adapter = source.adapter;
    e.texture_format = source.texture_format;
    return e;
}

} // namespace

int main(int argc, char** argv) {
    bool simulate_empty_adapter = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--simulate-empty-adapter") == 0) {
            simulate_empty_adapter = true;
        } else {
            std::cerr << "usage: pulp-vellum-gpu-adapter-probe [--simulate-empty-adapter]\n";
            return 64;
        }
    }

    probe::Evidence evidence;
    probe::Frame frame;
    bool captured = false;
    std::string error;
    if (vellum::app_host::register_native_dawn_bootstrap(&error)) {
        auto surface = vg::SkiaDawnSurface::create(
            {.width = probe::kSceneWidth, .height = probe::kSceneHeight, .scale = 1.0F}, &error);
        if (surface) {
            evidence = to_evidence(surface->evidence());
            if (evidence.available && surface->render(probe_scene(), &error)) {
                captured = surface->capture_rgba(frame.rgba, frame.width, frame.height, &error);
            }
        }
    }
    if (simulate_empty_adapter)
        evidence.adapter.clear();

    const auto result = probe::classify(evidence, captured ? &frame : nullptr);
    std::cout << probe::to_json(result) << '\n';
    if (result.verdict != probe::Verdict::pass && !error.empty())
        std::cerr << "pulp-vellum-gpu-adapter-probe: " << error << '\n';
    return probe::exit_code(result.verdict);
}
