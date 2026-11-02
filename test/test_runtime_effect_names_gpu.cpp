// Caller-supplied SkSL that declares names Graphite uses for its own
// runtime-effect parameters, drawn on a live offscreen Dawn + Skia Graphite
// surface.
//
// Graphite emits a runtime shader's `main(float2 xy)` as a helper whose
// coordinate parameter is named `pos`. Skia releases before chrome/m158 renamed
// the author's parameter but not the author's locals, so a local `pos` in
// `main` either redefined the parameter (the pipeline failed to compile and the
// null pipeline crashed inside insertRecording) or, in a nested block, shadowed
// the coordinates and rendered black. The linked Skia keeps author locals apart
// from that parameter, so every case here paints a horizontal red ramp,
// r = x / 64, and must match a control shader whose local is named `q`: locals
// named `pos`, `inColor` and `destColor` in `main` and in nested blocks,
// globals and uniforms named `pos`, helper-function locals, and a `main` whose
// own parameter is `pos`. These pin the linked Skia's behaviour, so a Skia
// update that brings the collision back fails here instead of in a user's
// editor.

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/canvas.hpp>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#if defined(PULP_HAS_SKIA) && defined(__APPLE__)
#include <pulp/render/gpu_surface.hpp>
#include <pulp/render/skia_surface.hpp>
#endif

#if defined(PULP_HAS_SKIA) && defined(__APPLE__)

using namespace pulp;

namespace {

constexpr uint32_t kCell = 64;

struct NamedShader {
    const char* label;
    const char* sksl;
};

// The control: a local that matches no Graphite parameter name.
constexpr NamedShader kControl{
    "control local q",
    "half4 main(float2 xy) { float2 q = xy; return half4(half(q.x / 64.0), 0, 0, 1); }"};

// Each must compile and render exactly like the control.
constexpr NamedShader kNameAlikes[] = {
    {"local pos in main",
     "half4 main(float2 xy) { float2 pos = xy; return half4(half(pos.x / 64.0), 0, 0, 1); }"},
    // A nested `pos` declared before a use of `xy`: if Graphite's coordinate
    // parameter shared the author's scope, the use would bind to this zero
    // local and the ramp would go black.
    {"nested-block pos shadowing xy",
     "half4 main(float2 xy) { half r = 0; { float2 pos = float2(0.0); "
     "r = half(xy.x / 64.0) + half(pos.y); } return half4(r, 0, 0, 1); }"},
    {"local inColor in main",
     "half4 main(float2 xy) { half4 inColor = half4(half(xy.x / 64.0), 0, 0, 1); "
     "return inColor; }"},
    {"local destColor in main",
     "half4 main(float2 xy) { half4 destColor = half4(half(xy.x / 64.0), 0, 0, 1); "
     "return destColor; }"},
    {"nested-block inColor and destColor",
     "half4 main(float2 xy) { half r = 0; { half4 inColor = half4(0); "
     "half4 destColor = half4(0); r = half(xy.x / 64.0) + inColor.r + destColor.r; } "
     "return half4(r, 0, 0, 1); }"},
    {"global const pos read in main",
     "const float2 pos = float2(16.0, 16.0); "
     "half4 main(float2 xy) { return half4(half((xy.x - pos.x + 16.0) / 64.0), 0, 0, 1); }"},
    // A uniform named pos keeps its own (zero) value; Graphite's coordinate
    // parameter does not shadow it.
    {"uniform pos read in main",
     "uniform float2 pos; "
     "half4 main(float2 xy) { return half4(half((xy.x + pos.x) / 64.0), 0, 0, 1); }"},
    {"global pos beside a uniform",
     "uniform float value; float2 pos = float2(16.0, 16.0); "
     "half4 main(float2 xy) { return half4(half((xy.x - pos.x + 16.0) / 64.0), 0, 0, 1); }"},
    {"main parameter named pos",
     "half4 main(float2 pos) { half r = 0; { float2 q = pos; r = half(q.x / 64.0); } "
     "return half4(r, 0, 0, 1); }"},
    {"locals in a helper function",
     "half4 ramp(float2 p) { float2 pos = p; half4 inColor = half4(half(pos.x / 64.0), 0, 0, 1); "
     "half4 destColor = inColor; return destColor; } "
     "half4 main(float2 xy) { return ramp(xy); }"},
};

struct Fixture {
    std::unique_ptr<render::GpuSurface> gpu;
    std::unique_ptr<render::SkiaSurface> skia;
};

Fixture make_fixture(uint32_t w, uint32_t h) {
    Fixture f;
    f.gpu = render::GpuSurface::create_dawn();
    if (!f.gpu)
        return f;
    render::GpuSurface::Config gpu_config{};
    gpu_config.width = w;
    gpu_config.height = h;
    gpu_config.native_surface_handle = nullptr;
    if (!f.gpu->initialize(gpu_config)) {
        f.gpu.reset();
        return f;
    }
    render::SkiaSurface::Config skia_config{};
    skia_config.width = w;
    skia_config.height = h;
    skia_config.scale_factor = 1.0f;
    f.skia = render::SkiaSurface::create(*f.gpu, skia_config);
    return f;
}

struct Frame {
    std::vector<uint8_t> px;
    uint32_t w = 0;
    uint32_t h = 0;
    std::vector<bool> drawn;
};

// Paints every shader into its own kCell-wide column over a blue backdrop, so
// a shader that did not run leaves blue and one that ran leaves a red ramp.
bool paint(Fixture& f, const std::vector<NamedShader>& shaders, Frame& out) {
    if (!f.gpu->begin_frame())
        return false;
    auto* canvas = f.skia->begin_frame();
    if (!canvas) {
        f.gpu->end_frame();
        return false;
    }
    canvas->set_fill_color(canvas::Color::rgba8(0, 0, 255, 255));
    canvas->fill_rect(0, 0, static_cast<float>(kCell * shaders.size()), kCell);
    canvas::Canvas::ShaderUniforms uniforms{};
    out.drawn.clear();
    for (size_t i = 0; i < shaders.size(); ++i) {
        out.drawn.push_back(canvas->draw_with_sksl(shaders[i].sksl, static_cast<float>(i * kCell),
                                                   0, kCell, kCell, uniforms));
    }
    f.skia->end_frame();
    f.gpu->end_frame();
    return f.skia->read_current_rgba(out.px, out.w, out.h);
}

const uint8_t* pixel(const Frame& frame, uint32_t x, uint32_t y) {
    return frame.px.data() + (static_cast<size_t>(y) * frame.w + x) * 4;
}

} // namespace

TEST_CASE("Runtime-effect locals named like Graphite's parameters render as written",
          "[gpu][skia][sksl][runtime-effect]") {
    std::vector<NamedShader> shaders{kControl};
    for (const auto& s : kNameAlikes)
        shaders.push_back(s);

    for (const auto& s : shaders) {
        INFO(s.label);
        CHECK(canvas::Canvas::compile_sksl(s.sksl).empty());
    }

    auto f = make_fixture(static_cast<uint32_t>(kCell * shaders.size()), kCell);
    if (!f.gpu || !f.skia || !f.skia->is_available())
        SKIP("Dawn/Graphite unavailable on this host.");

    Frame frame;
    if (!paint(f, shaders, frame))
        SKIP("GPU readback failed (no adapter).");
    REQUIRE(frame.w == kCell * shaders.size());
    REQUIRE(frame.h == kCell);

    // The control must have produced the ramp, or the instrument is blind.
    const uint8_t* control_mid = pixel(frame, kCell / 2 + kCell / 4, kCell / 2);
    if (control_mid[2] > 200 && control_mid[0] < 8)
        SKIP("Offscreen GPU composited no shader output (no real adapter).");
    REQUIRE(frame.drawn[0]);
    // r = 48 / 64 = 0.75 at the sampled column; blue must be gone.
    REQUIRE(std::abs(int(control_mid[0]) - 191) <= 3);
    REQUIRE(control_mid[2] <= 3);

    for (size_t i = 1; i < shaders.size(); ++i) {
        INFO(shaders[i].label);
        CHECK(frame.drawn[i]);
        for (uint32_t local_x : {8u, 32u, 56u}) {
            const uint32_t x = static_cast<uint32_t>(i * kCell) + local_x;
            const uint8_t* got = pixel(frame, x, kCell / 2);
            // Must match the control column; the blue backdrop means the
            // shader was refused or dropped, black means `pos` shadowed the
            // coordinates.
            const uint8_t* want = pixel(frame, local_x, kCell / 2);
            INFO("local x " << local_x << ": got rgba(" << int(got[0]) << "," << int(got[1]) << ","
                            << int(got[2]) << ") want rgba(" << int(want[0]) << "," << int(want[1])
                            << "," << int(want[2]) << ")");
            CHECK(std::abs(int(got[0]) - int(want[0])) <= 1);
            CHECK(std::abs(int(got[1]) - int(want[1])) <= 1);
            CHECK(std::abs(int(got[2]) - int(want[2])) <= 1);
        }
    }
}

#endif
