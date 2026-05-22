// test_plugin_editor_headless_gpu.cpp — GPU view-host-in-plugins (P1 proof)
//
// Proves the plugin-editor GPU path end to end WITHOUT a DAW or a window:
//
//   1. Capability selection (decide_gpu_host): a Processor whose
//      create_view() returns a view with set_requires_gpu_host(true) makes
//      the adapters auto-select the GPU host (mode="custom", use_gpu=true);
//      an AutoUi fallback (create_view()==nullptr) does NOT (mode="autoui",
//      use_gpu=false); and PULP_DISABLE_PLUGIN_GPU forces CPU. This is the
//      scream-guard's selection logic — it's what prevents the silent
//      AutoUi/CPU fallback that triggered this work.
//
//   2. Structural assertion (ViewInspector::to_json): the mounted tree has
//      the editor's own labels and is NOT the AutoUi "Parameters" grid.
//
//   3. Nonblank GPU digest: render the editor view through an offscreen
//      Dawn+Skia surface (native_surface_handle=nullptr) and read it back via
//      SkiaSurface::read_current_rgba — assert pixels were painted beyond the
//      cleared background. Mirrors test_font_rendering_goldens_gpu.cpp; soft-
//      skips when no Dawn adapter is present (CI lane without a GPU).
//
// Gated PULP_HAS_SKIA && APPLE && PULP_ENABLE_GPU at compile time (CMake);
// soft-skips on Dawn-init failure at run time.
//
// Tag: [gpu][skia][plugin-gpu-host][issue-2700-followup]

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/canvas.hpp>
#include <pulp/format/gpu_host_select.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/view_bridge.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/inspector.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#if defined(PULP_HAS_SKIA) && defined(__APPLE__)
#include <pulp/render/gpu_surface.hpp>
#include <pulp/render/skia_surface.hpp>
#endif

using namespace pulp;

namespace {

// Distinctive labels — the structural assertion looks for these and the
// absence of the AutoUi "Parameters" title to prove the real editor mounted.
constexpr const char* kEditorLabels[] = {"CHAINER", "OSC", "X-OVER", "LIMIT"};

constexpr uint32_t kW = 480;
constexpr uint32_t kH = 320;
// The editor view paints this background; non-background pixels = painted UI.
constexpr uint8_t kBgR = 30, kBgG = 30, kBgB = 46;

// A Processor whose create_view() returns a GPU-backed scripted-style editor.
class GpuEditorProcessor : public format::Processor {
public:
    bool return_view = true;  // false → exercise the AutoUi fallback path

    format::PluginDescriptor descriptor() const override {
        return {"GpuEditor", "Acme", "com.acme.gpueditor", "1.0.0",
                format::PluginCategory::Effect};
    }
    void define_parameters(state::StateStore& s) override {
        s.add_parameter({1, "Level", "dB", {-60.0f, 12.0f, 0.0f}});
    }
    void prepare(const format::PrepareContext&) override {}
    void process(audio::BufferView<float>&, const audio::BufferView<const float>&,
                 midi::MidiBuffer&, midi::MidiBuffer&,
                 const format::ProcessContext&) override {}

    format::ViewSize view_size() const override {
        return {kW, kH, 320, 240, 1024, 768};
    }

    std::unique_ptr<view::View> create_view() override {
        if (!return_view) return nullptr;  // → ViewBridge builds AutoUi
        auto root = std::make_unique<view::View>();
        root->set_requires_gpu_host(true);
        root->set_background_color(canvas::Color::rgba8(kBgR, kBgG, kBgB));
        root->flex().direction = view::FlexDirection::column;
        root->flex().padding = 16;
        root->flex().gap = 10;
        for (const char* text : kEditorLabels) {
            auto label = std::make_unique<view::Label>(text);
            label->set_font_size(20.0f);
            label->flex().preferred_height = 28;
            root->add_child(std::move(label));
        }
        return root;
    }
};

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("plugin-gpu-host: a requires_gpu_host editor auto-selects GPU",
          "[gpu][plugin-gpu-host]") {
    GpuEditorProcessor p;
    state::StateStore store;
    p.set_state_store(&store);
    p.define_parameters(store);

    format::ViewBridge bridge(p, store);
    std::string err;
    REQUIRE(bridge.open(&err));
    bridge.notify_attached();
    REQUIRE(bridge.view() != nullptr);
    REQUIRE(bridge.view()->requires_gpu_host());

    // Make sure the env opt-out isn't set from the environment.
    ::unsetenv("PULP_DISABLE_PLUGIN_GPU");
    auto decision = format::decide_gpu_host(bridge);
    REQUIRE(decision.wants_gpu);
    REQUIRE(decision.use_gpu);
    REQUIRE(std::string(decision.mode) == "custom");

    SECTION("PULP_DISABLE_PLUGIN_GPU forces CPU but still records intent") {
        ::setenv("PULP_DISABLE_PLUGIN_GPU", "1", 1);
        auto off = format::decide_gpu_host(bridge);
        CHECK(off.wants_gpu);       // capability unchanged
        CHECK_FALSE(off.use_gpu);   // request suppressed
        ::unsetenv("PULP_DISABLE_PLUGIN_GPU");
    }
}

TEST_CASE("plugin-gpu-host: AutoUi fallback does NOT request GPU",
          "[gpu][plugin-gpu-host]") {
    // This is the scream-guard's core invariant: an editor that genuinely
    // wants AutoUi/CPU must NOT request the GPU host (and vice-versa).
    GpuEditorProcessor p;
    p.return_view = false;  // create_view() → nullptr → AutoUi
    state::StateStore store;
    p.set_state_store(&store);
    p.define_parameters(store);

    format::ViewBridge bridge(p, store);
    std::string err;
    REQUIRE(bridge.open(&err));
    REQUIRE(bridge.view() != nullptr);
    REQUIRE_FALSE(bridge.view()->requires_gpu_host());

    ::unsetenv("PULP_DISABLE_PLUGIN_GPU");
    auto decision = format::decide_gpu_host(bridge);
    REQUIRE_FALSE(decision.wants_gpu);
    REQUIRE_FALSE(decision.use_gpu);
    REQUIRE(std::string(decision.mode) == "autoui");
}

TEST_CASE("plugin-gpu-host: editor tree is structurally not AutoUi",
          "[gpu][plugin-gpu-host]") {
    GpuEditorProcessor p;
    state::StateStore store;
    p.set_state_store(&store);
    p.define_parameters(store);

    format::ViewBridge bridge(p, store);
    std::string err;
    REQUIRE(bridge.open(&err));

    const std::string json = view::ViewInspector::to_json(*bridge.view());
    for (const char* label : kEditorLabels) {
        INFO("expected editor label: " << label);
        REQUIRE(contains(json, label));
    }
    // AutoUi's root title is the "Parameters" label — the real editor has none.
    REQUIRE_FALSE(contains(json, "Parameters"));
}

#if defined(PULP_HAS_SKIA) && defined(__APPLE__)

namespace {

struct GpuFixture {
    std::unique_ptr<render::GpuSurface> gpu;
    std::unique_ptr<render::SkiaSurface> skia;
    bool ready() const { return gpu && skia && skia->is_available(); }
};

GpuFixture make_offscreen_fixture(uint32_t w, uint32_t h) {
    GpuFixture f;
    f.gpu = render::GpuSurface::create_dawn();
    if (!f.gpu) return f;
    render::GpuSurface::Config gpu_config{};
    gpu_config.width = w;
    gpu_config.height = h;
    gpu_config.native_surface_handle = nullptr;  // headless / offscreen
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

// Count pixels that differ from the editor's known background — i.e. pixels
// the UI actually painted (labels). A blank/failed frame returns ~0.
uint32_t count_non_background(const std::vector<uint8_t>& px, uint32_t w, uint32_t h) {
    const size_t need = static_cast<size_t>(w) * h * 4u;
    if (px.size() < need) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < w * h; ++i) {
        const uint8_t r = px[i * 4 + 0];
        const uint8_t g = px[i * 4 + 1];
        const uint8_t b = px[i * 4 + 2];
        const int dr = std::abs(static_cast<int>(r) - kBgR);
        const int dg = std::abs(static_cast<int>(g) - kBgG);
        const int db = std::abs(static_cast<int>(b) - kBgB);
        if (dr + dg + db > 24) ++n;
    }
    return n;
}

} // namespace

TEST_CASE("plugin-gpu-host: editor view paints nonblank GPU frame (offscreen)",
          "[gpu][skia][plugin-gpu-host]") {
    auto f = make_offscreen_fixture(kW, kH);
    if (!f.ready()) {
        SUCCEED("Dawn/Graphite unavailable on this host — offscreen GPU proof skipped.");
        return;
    }

    GpuEditorProcessor p;
    state::StateStore store;
    p.set_state_store(&store);
    p.define_parameters(store);
    format::ViewBridge bridge(p, store);
    std::string err;
    REQUIRE(bridge.open(&err));
    view::View* root = bridge.view();
    REQUIRE(root != nullptr);

    REQUIRE(f.gpu->begin_frame());
    auto* canvas = f.skia->begin_frame();
    REQUIRE(canvas != nullptr);
    root->set_bounds({0, 0, static_cast<float>(kW), static_cast<float>(kH)});
    root->layout_children();
    canvas->set_fill_color(canvas::Color::rgba8(kBgR, kBgG, kBgB));
    canvas->fill_rect(0, 0, static_cast<float>(kW), static_cast<float>(kH));
    root->paint_all(*canvas);

    std::vector<uint8_t> pixels;
    uint32_t pw = 0, ph = 0;
    const bool read = f.skia->read_current_rgba(pixels, pw, ph);
    f.skia->end_frame();
    f.gpu->end_frame();

    if (!read) {
        SUCCEED("GPU readback failed (no adapter) — nonblank proof skipped.");
        return;
    }
    const uint32_t painted = count_non_background(pixels, pw, ph);
    INFO("non-background pixels: " << painted << " of " << (pw * ph));
    // Four labels at 20px over a 480x320 surface paint thousands of glyph
    // pixels; require a conservative floor well above readback noise.
    REQUIRE(painted > 200u);
}

#endif // PULP_HAS_SKIA && __APPLE__
