// test_text_raster_policy.cpp — the glyph preblend every Skia surface is
// created with (pulp/canvas/text_raster_policy.hpp).
//
// Contrast and gamma live on the SURFACE, so the property that matters is that
// every surface a frame's text can pass through carries the same one. A layer
// or scene target created with defaults caches its text at a different weight
// from the same text painted directly, and nothing else fails when it does.
//
// The weight claims (a `chromium` surface paints `auto` text within a few
// percent of a `legacy` one, and a `legacy` surface is Chromium's `auto`) are
// macOS measurements: CoreText rasterizes through Skia there. They are asserted
// on Apple only; the plumbing assertions run everywhere.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/text_raster_policy.hpp>

#include <array>

#ifdef PULP_HAS_SKIA
#include <pulp/canvas/bundled_fonts.hpp>
#include <pulp/canvas/skia_canvas.hpp>
#include <pulp/canvas/skia_text_raster_policy.hpp>
#include <pulp/render/headless_surface.hpp>
#include <pulp/render/skia_surface.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/theme_presets.hpp>
#include <pulp/view/widget_gallery.hpp>

#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkSurface.h"
#include "include/core/SkSurfaceProps.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>
#endif

using namespace pulp;
using canvas::FontSmoothing;
using canvas::TextRasterPolicy;

TEST_CASE("text raster policy turns chromium when any text node is antialiased",
          "[canvas][text-raster]") {
    using canvas::select_text_raster_policy;
    CHECK(select_text_raster_policy({}) == TextRasterPolicy::legacy);

    const std::array<FontSmoothing, 3> all_auto{FontSmoothing::auto_smoothing,
                                                FontSmoothing::auto_smoothing,
                                                FontSmoothing::auto_smoothing};
    CHECK(select_text_raster_policy(all_auto) == TextRasterPolicy::legacy);

    // A body on `auto` with one `antialiased` span: the surface flips. Taking
    // the root's mode instead would paint the span about 21% too heavy.
    const std::array<FontSmoothing, 3> one_antialiased_span{
        FontSmoothing::auto_smoothing, FontSmoothing::antialiased, FontSmoothing::auto_smoothing};
    CHECK(select_text_raster_policy(one_antialiased_span) == TextRasterPolicy::chromium);

    const std::array<FontSmoothing, 1> antialiased_body{FontSmoothing::antialiased};
    CHECK(select_text_raster_policy(antialiased_body) == TextRasterPolicy::chromium);
}

TEST_CASE("text raster tuples are the measured contrast and gamma", "[canvas][text-raster]") {
    CHECK(canvas::text_raster_tuple(TextRasterPolicy::chromium).contrast == 0.0f);
    CHECK(canvas::text_raster_tuple(TextRasterPolicy::chromium).gamma == 1.0f);
    CHECK(canvas::text_raster_tuple(TextRasterPolicy::legacy).contrast == 0.5f);
    CHECK(canvas::text_raster_tuple(TextRasterPolicy::legacy).gamma == 0.0f);
}

#ifdef PULP_HAS_SKIA

namespace {

constexpr int kW = 120;
constexpr int kH = 32;
constexpr float kScale = 2.0f;
constexpr const char* kFamily = "Text Raster Policy Mono";

void register_fixture_font() {
    static const bool registered = canvas::register_font_file(PULP_TEST_JETBRAINS_TTF, kFamily);
    REQUIRE(registered);
}

sk_sp<SkSurface> make_target(TextRasterPolicy policy, int w = kW, int h = kH) {
    const SkImageInfo info = SkImageInfo::MakeN32Premul(
        static_cast<int>(w * kScale), static_cast<int>(h * kScale), SkColorSpace::MakeSRGB());
    const SkSurfaceProps props = canvas::surface_props_for(policy);
    auto surface = SkSurfaces::Raster(info, &props);
    REQUIRE(surface);
    surface->getCanvas()->scale(kScale, kScale);
    return surface;
}

void fill_background(canvas::Canvas& c) {
    c.set_fill_color(canvas::Color::rgba8(14, 16, 19));
    c.fill_rect(0, 0, kW, kH);
}

// A Spectr toolbar label: JetBrains Mono 10px, 1px tracking, 85% white.
void draw_label(canvas::Canvas& c) {
    c.set_font_full(kFamily, 10.0f, 400, 0, 1.0f);
    c.set_fill_color(canvas::Color::rgba8(255, 255, 255, 217));
    c.fill_text("SCULPT", 8.0f, 20.0f);
}

std::vector<std::uint32_t> pixels_of(SkSurface& surface) {
    SkPixmap pm;
    REQUIRE(surface.peekPixels(&pm));
    std::vector<std::uint32_t> out(static_cast<size_t>(pm.width()) * pm.height());
    for (int y = 0; y < pm.height(); ++y)
        for (int x = 0; x < pm.width(); ++x)
            out[static_cast<size_t>(y) * pm.width() + x] = *pm.addr32(x, y);
    return out;
}

std::vector<std::uint32_t> render_direct(TextRasterPolicy policy) {
    auto surface = make_target(policy);
    canvas::SkiaCanvas c(surface->getCanvas());
    fill_background(c);
    draw_label(c);
    return pixels_of(*surface);
}

// The label recorded into a cacheable retained layer on one frame canvas and
// composited from the store on the NEXT frame canvas, which is how a cached
// subtree reaches the screen.
std::vector<std::uint32_t> render_through_retained_layer(TextRasterPolicy policy) {
    auto surface = make_target(policy);
    auto store = canvas::SkiaCanvas::create_retained_layer_store();
    canvas::Canvas::LayerHandle cached;
    {
        canvas::SkiaCanvas first(surface->getCanvas(), nullptr, store);
        cached = first.begin_layer({0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH)},
                                   /*cacheable=*/true);
        draw_label(first);
        cached = first.end_layer();
        REQUIRE(cached);
    }
    canvas::SkiaCanvas second(surface->getCanvas(), nullptr, store);
    fill_background(second);
    REQUIRE(second.layer_valid(cached));
    second.draw_layer(cached);
    return pixels_of(*surface);
}

int max_channel_delta(const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b) {
    REQUIRE(a.size() == b.size());
    int worst = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        for (int shift = 0; shift < 32; shift += 8) {
            const int da = static_cast<int>((a[i] >> shift) & 0xff);
            const int db = static_cast<int>((b[i] >> shift) & 0xff);
            worst = std::max(worst, std::abs(da - db));
        }
    }
    return worst;
}

// Glyph ink above the flat background: the summed luminance excess. Ink ratios
// are position-blind, so they compare weight and nothing else.
double ink_of(const std::vector<std::uint32_t>& px) {
    auto luma = [](std::uint32_t p) {
        const double c0 = p & 0xff, c1 = (p >> 8) & 0xff, c2 = (p >> 16) & 0xff;
        return (c0 + c1 + c2) / 3.0;
    };
    const double bg = luma(px.front());
    double ink = 0.0;
    for (const std::uint32_t p : px)
        ink += std::max(0.0, luma(p) - bg);
    return ink;
}

} // namespace

TEST_CASE("legacy surface props are the props a surface gets without any",
          "[canvas][text-raster]") {
    CHECK(canvas::surface_props_for(TextRasterPolicy::legacy) == SkSurfaceProps());
    const SkSurfaceProps chromium = canvas::surface_props_for(TextRasterPolicy::chromium);
    CHECK(chromium.textContrast() == 0.0f);
    CHECK(chromium.textGamma() == 1.0f);
    CHECK(canvas::text_raster_policy_of(chromium) == TextRasterPolicy::chromium);
    CHECK(canvas::text_raster_policy_of(SkSurfaceProps()) == TextRasterPolicy::legacy);
}

TEST_CASE("text through a retained layer matches direct paint under each policy",
          "[canvas][text-raster][layer-cache]") {
    register_fixture_font();
    for (const TextRasterPolicy policy : {TextRasterPolicy::legacy, TextRasterPolicy::chromium}) {
        INFO("policy " << static_cast<int>(policy));
        const auto direct = render_direct(policy);
        const auto layered = render_through_retained_layer(policy);
        REQUIRE(ink_of(direct) > 0.0);
        CHECK(ink_of(layered) == Catch::Approx(ink_of(direct)).epsilon(0.01));
        CHECK(max_channel_delta(direct, layered) <= 1);
    }
}

#if defined(__APPLE__)
TEST_CASE("a chromium surface changes how text rasterizes", "[canvas][text-raster]") {
    // Without this, the byte-identity above would also pass for a policy that
    // never reached the glyph rasterizer. With the painter's hinting, the
    // linear preblend leaves CoreText smoothing on and adds about 2% ink;
    // the lighter `antialiased` weight needs hinting none as well.
    register_fixture_font();
    const auto legacy = render_direct(TextRasterPolicy::legacy);
    const auto chromium = render_direct(TextRasterPolicy::chromium);
    CHECK(chromium != legacy);
    CHECK(ink_of(chromium) / ink_of(legacy) > 1.005);
}

TEST_CASE("an auto span on a chromium surface stays within the ink band", "[canvas][text-raster]") {
    // A body set `antialiased` with one span on `auto`: the surface is chromium,
    // and the span keeps its own hinting. A legacy surface paints `auto` text
    // exactly as Chromium does, so it is the span's reference.
    const std::array<FontSmoothing, 2> page{FontSmoothing::antialiased,
                                            FontSmoothing::auto_smoothing};
    REQUIRE(canvas::select_text_raster_policy(page) == TextRasterPolicy::chromium);

    register_fixture_font();
    const double reference = ink_of(render_direct(TextRasterPolicy::legacy));
    const double span = ink_of(render_direct(canvas::select_text_raster_policy(page)));
    CHECK(span / reference == Catch::Approx(1.0).margin(0.03));
}
#endif

TEST_CASE("the widget gallery renders unchanged under the default policy",
          "[canvas][text-raster][gallery]") {
    // Every non-imported UI keeps legacy props. The reference is painted the
    // way the raster screenshot backend paints, on a surface created with NO
    // props, so a change of default policy (or of what legacy means) fails
    // here.
    if (!view::raw_rgba_render_available())
        SKIP("no raw RGBA raster backend in this build");
    const view::ThemePreset* preset = view::find_preset("ink-signal");
    REQUIRE(preset != nullptr);
    auto gallery = view::build_widget_gallery(view::theme_from_preset(*preset, true));
    REQUIRE(gallery != nullptr);
    const auto w = static_cast<std::uint32_t>(gallery->bounds().width);
    const auto h = static_cast<std::uint32_t>(std::min(gallery->bounds().height, 900.0f));

    std::uint32_t pw = 0, ph = 0;
    const auto shipped = view::render_to_rgba(*gallery, w, h, 1.0f, &pw, &ph);
    REQUIRE_FALSE(shipped.empty());

    const SkImageInfo info =
        SkImageInfo::Make(static_cast<int>(pw), static_cast<int>(ph), kRGBA_8888_SkColorType,
                          kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
    auto reference_surface = SkSurfaces::Raster(info);
    REQUIRE(reference_surface);
    {
        canvas::SkiaCanvas c(reference_surface->getCanvas());
        c.set_fill_color(canvas::Color::rgba8(30, 30, 46));
        c.fill_rect(0, 0, static_cast<float>(w), static_cast<float>(h));
        gallery->set_bounds({0, 0, static_cast<float>(w), static_cast<float>(h)});
        gallery->layout_children();
        gallery->paint_all(c);
        view::View::paint_overlays(c, gallery.get());
    }
    std::vector<std::uint8_t> reference(shipped.size());
    REQUIRE(reference_surface->readPixels(
        SkPixmap(info, reference.data(), static_cast<size_t>(pw) * 4u), 0, 0));
    CHECK(reference == shipped);

    const auto chromium =
        view::render_to_rgba(*gallery, w, h, 1.0f, &pw, &ph, TextRasterPolicy::chromium);
    REQUIRE_FALSE(chromium.empty());
#if defined(__APPLE__)
    CHECK(chromium != shipped);
#endif
}

TEST_CASE("a GPU surface carries its text raster policy into retained layers",
          "[render][skia][text-raster][layer-cache]") {
    register_fixture_font();
    std::vector<std::vector<std::uint8_t>> frames;
    for (const TextRasterPolicy policy : {TextRasterPolicy::legacy, TextRasterPolicy::chromium}) {
        render::HeadlessSurface::Config cfg;
        cfg.width = kW;
        cfg.height = kH;
        cfg.scale_factor = kScale;
        cfg.clear_r = 14;
        cfg.clear_g = 16;
        cfg.clear_b = 19;
        cfg.text_raster_policy = policy;
        std::string err;
        auto surface = render::HeadlessSurface::create(cfg, &err);
        if (!surface || !surface->is_ready())
            SKIP("no GPU surface: " << err);

        const auto direct = surface->render_rgba([](canvas::Canvas& c) { draw_label(c); });
        const auto layered = surface->render_rgba([](canvas::Canvas& c) {
            c.begin_layer({0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH)},
                          /*cacheable=*/true);
            draw_label(c);
            const auto handle = c.end_layer();
            c.draw_layer(handle);
        });
        REQUIRE_FALSE(direct.empty());
        REQUIRE(direct.pixels.size() == layered.pixels.size());
        int worst = 0;
        for (size_t i = 0; i < direct.pixels.size(); ++i)
            worst = std::max(worst, std::abs(static_cast<int>(direct.pixels[i]) -
                                             static_cast<int>(layered.pixels[i])));
        INFO("policy " << static_cast<int>(policy));
        CHECK(worst <= 1);
        frames.push_back(direct.pixels);
    }
#if defined(__APPLE__)
    CHECK(frames[0] != frames[1]);
#endif
}

namespace {

// One frame of the label on `skia`, read back. With persistent scene on, the
// frame is drawn into the retained scene target instead of the offscreen one.
std::vector<std::uint8_t> skia_surface_frame(render::GpuSurface& gpu, render::SkiaSurface& skia) {
    REQUIRE(gpu.begin_frame());
    auto* c = skia.begin_frame();
    REQUIRE(c != nullptr);
    fill_background(*c);
    draw_label(*c);
    std::vector<std::uint8_t> pixels;
    std::uint32_t w = 0, h = 0;
    REQUIRE(skia.read_current_rgba(pixels, w, h));
    skia.end_frame();
    gpu.end_frame();
    return pixels;
}

} // namespace

TEST_CASE("the persistent scene target carries the surface's text raster policy",
          "[render][skia][text-raster]") {
    register_fixture_font();
    auto gpu = render::GpuSurface::create_dawn();
    if (!gpu)
        SKIP("no GPU compute device available");
    render::GpuSurface::Config gpu_config{};
    gpu_config.width = kW;
    gpu_config.height = kH;
    if (!gpu->initialize(gpu_config))
        SKIP("Dawn adapter initialization failed");
    if (gpu->adapter_info().backend_type == "Null")
        SKIP("Dawn Null backend cannot produce raster output");

    std::vector<std::vector<std::uint8_t>> offscreen_frames;
    for (const TextRasterPolicy policy : {TextRasterPolicy::legacy, TextRasterPolicy::chromium}) {
        render::SkiaSurface::Config config{};
        config.width = kW;
        config.height = kH;
        config.scale_factor = kScale;
        config.text_raster_policy = policy;
        auto skia = render::SkiaSurface::create(*gpu, config);
        if (!skia || !skia->is_available())
            SKIP("Skia Graphite surface unavailable");

        const auto offscreen = skia_surface_frame(*gpu, *skia);
        if (!skia->set_persistent_scene(true))
            SKIP("persistent scene unsupported");
        const auto scene = skia_surface_frame(*gpu, *skia);
        INFO("policy " << static_cast<int>(policy));
        REQUIRE(offscreen.size() == scene.size());
        CHECK(offscreen == scene);
        offscreen_frames.push_back(offscreen);
    }
#if defined(__APPLE__)
    CHECK(offscreen_frames[0] != offscreen_frames[1]);
#endif
}

TEST_CASE("changing a SkiaSurface's text raster policy is reported back",
          "[render][skia][text-raster]") {
    auto gpu = render::GpuSurface::create_dawn();
    if (!gpu)
        SKIP("no GPU compute device available");
    render::GpuSurface::Config gpu_config{};
    gpu_config.width = kW;
    gpu_config.height = kH;
    if (!gpu->initialize(gpu_config))
        SKIP("Dawn adapter initialization failed");
    render::SkiaSurface::Config config{};
    config.width = kW;
    config.height = kH;
    config.text_raster_policy = TextRasterPolicy::chromium;
    auto skia = render::SkiaSurface::create(*gpu, config);
    if (!skia || !skia->is_available())
        SKIP("Skia Graphite surface unavailable");
    CHECK(skia->text_raster_policy() == TextRasterPolicy::chromium);
    skia->set_text_raster_policy(TextRasterPolicy::legacy);
    CHECK(skia->text_raster_policy() == TextRasterPolicy::legacy);
}

#endif // PULP_HAS_SKIA
