#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/canvas.hpp>
#include <pulp/view/chart_shader_presets.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>

#include <algorithm>
#include <clocale>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <regex>
#include <string>
#include <vector>

using namespace pulp::view;
using pulp::canvas::Canvas;

namespace {

// Paints one chart shape with a preset, offset by half a pixel so pixel
// (i, j) samples the chart at p = (i - w/2, j - h/2) exactly.
class PresetSwatch : public View {
  public:
    PresetSwatch(Canvas::SDFShape shape, ChartShaderPreset preset, Canvas::SDFStyle style)
        : shape_(shape), preset_(std::move(preset)), style_(style) {}
    void paint(pulp::canvas::Canvas& canvas) override {
        Canvas::ShaderDrawOptions options;
        options.reach = preset_.reach;
        drawn_ = canvas.draw_sdf_shape_with_shader(shape_, 0.5f, 0.5f, bounds().width,
                                                   bounds().height, style_, preset_.sksl, options);
    }
    bool drawn() const {
        return drawn_;
    }

  private:
    Canvas::SDFShape shape_;
    ChartShaderPreset preset_;
    Canvas::SDFStyle style_;
    bool drawn_ = false;
};

// A horizontal flat segment, 128 x 32, stroke 16: the centre line is row 16
// and u = cross / halfWidth = k / 8 lies on row 16 - k (positive u is up).
struct SegmentRender {
    std::vector<uint8_t> rgba;
    bool drawn = false;

    float luma(int x, int y) const {
        const auto* p = &rgba[(static_cast<size_t>(y) * 128 + static_cast<size_t>(x)) * 4];
        return 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
    }
    float alpha(int x, int y) const {
        return rgba[(static_cast<size_t>(y) * 128 + static_cast<size_t>(x)) * 4 + 3];
    }
    // Luminance on the +u (top) side minus the -u (bottom) side, at |u| = k/8.
    float top_minus_bottom(int k) const {
        return luma(64, 16 - k) - luma(64, 16 + k);
    }
};

SegmentRender render_segment(const std::string& name, const ChartShaderPresetOptions& options,
                             Canvas::SDFShape shape = Canvas::SDFShape::flat_segment) {
    auto preset = make_chart_shader_preset(name, options);
    INFO(preset.error);
    REQUIRE(preset.error.empty());
    Canvas::SDFStyle style;
    style.stroke_width = 16.0f;
    // The swatch is the render root, so no layout pass resizes it.
    PresetSwatch swatch(shape, preset, style);
    swatch.set_bounds({0, 0, 128, 32});
    SegmentRender out;
    uint32_t w = 0, h = 0;
    out.rgba = render_to_rgba(swatch, 128, 32, 1.0f, &w, &h);
    REQUIRE(w == 128);
    REQUIRE(h == 32);
    out.drawn = swatch.drawn();
    return out;
}

ChartShaderPresetOptions lit_from(float angle_deg) {
    ChartShaderPresetOptions options;
    options.light_angle_deg = angle_deg;
    return options;
}

} // namespace

TEST_CASE("chart shader presets are named and refuse what they cannot build",
          "[view][shader][chart-preset]") {
    const auto& names = chart_shader_preset_names();
    REQUIRE(names.size() == 6);
    for (const auto& name : names) {
        const auto preset = make_chart_shader_preset(name);
        INFO(name << ": " << preset.error);
        REQUIRE(preset.error.empty());
        REQUIRE(preset.sksl.find("half4 shade(PulpChart g)") != std::string::npos);
    }
    REQUIRE(make_chart_shader_preset("center-highlight").error.empty());

    const auto unknown = make_chart_shader_preset("chrome");
    REQUIRE(unknown.sksl.empty());
    REQUIRE(unknown.error.find("'chrome'") != std::string::npos);
    REQUIRE(unknown.error.find("light-pipe") != std::string::npos);

    ChartShaderPresetOptions bad;
    bad.bevel_width = 0.0f;
    REQUIRE(make_chart_shader_preset("bevel", bad).error.find("bevel width") != std::string::npos);
    bad = {};
    bad.light_elevation_deg = 0.0f;
    REQUIRE(make_chart_shader_preset("tube", bad).error.find("elevation") != std::string::npos);
    bad = {};
    bad.glow_sigma = -1.0f;
    REQUIRE(make_chart_shader_preset("glow", bad).error.find("sigma") != std::string::npos);

    // The glow's light lives outside the band, so it declares the reach that
    // holds its falloff; every other preset stays inside the band.
    // The reach ends where the halo drops under one 8-bit step, and is never
    // shorter than three sigma.
    ChartShaderPresetOptions glow;
    glow.glow_sigma = 5.0f;
    for (float intensity : {0.5f, 1.0f, 4.0f}) {
        glow.intensity = intensity;
        const float reach = make_chart_shader_preset("glow", glow).reach;
        INFO("intensity " << intensity << " reach " << reach);
        REQUIRE(reach >= 15.0f);
        REQUIRE(intensity * std::exp(-reach * reach / (2.0f * 25.0f)) <= 1.0f / 255.0f + 1e-6f);
    }
    glow.intensity = 1.0f;
    REQUIRE(make_chart_shader_preset("bevel", glow).reach == 0.0f);
    glow.glow_sigma = 0.0f;
    REQUIRE(make_chart_shader_preset("glow", glow).reach == 0.0f);
    // An unbounded sigma would grow the draw rect without limit.
    glow.glow_sigma = kChartGlowSigmaMax;
    REQUIRE(make_chart_shader_preset("glow", glow).error.empty());
    glow.glow_sigma = kChartGlowSigmaMax * 2.0f;
    REQUIRE(make_chart_shader_preset("glow", glow).error.find("sigma") != std::string::npos);

    // Colour channels outside [0, 1] are clamped before they reach the sRGB
    // decode, where a negative base would produce NaN.
    ChartShaderPresetOptions out_of_range;
    out_of_range.color = pulp::canvas::Color::rgba(-1.0f, 2.0f, 0.5f, 1.0f);
    ChartShaderPresetOptions clamped;
    clamped.color = pulp::canvas::Color::rgba(0.0f, 1.0f, 0.5f, 1.0f);
    REQUIRE(make_chart_shader_preset("bevel", out_of_range).sksl ==
            make_chart_shader_preset("bevel", clamped).sksl);

    // Small values survive as themselves rather than rounding to zero: the
    // halo's 2 * sigma^2 divisor is baked as about 2e-06, not 0.000000.
    ChartShaderPresetOptions faint;
    faint.glow_sigma = 0.001f;
    const auto faint_sksl = make_chart_shader_preset("glow", faint).sksl;
    std::smatch divisor;
    REQUIRE(
        std::regex_search(faint_sksl, divisor, std::regex(R"(exp\(-g\.d \* g\.d / ([^)]+)\))")));
    INFO("baked divisor " << divisor[1].str());
    REQUIRE(std::stod(divisor[1].str()) == Catch::Approx(2e-6).epsilon(1e-3));

    ChartProfile profile = ChartProfile::round;
    REQUIRE(parse_chart_profile("chamfer", profile));
    REQUIRE(profile == ChartProfile::chamfer);
    REQUIRE_FALSE(parse_chart_profile("square", profile));
}

TEST_CASE("chart shader presets compile on every charted shape", "[view][shader][chart-preset]") {
#ifdef PULP_HAS_SKIA
    const Canvas::SDFShape shapes[] = {
        Canvas::SDFShape::flat_arc,     Canvas::SDFShape::ring,
        Canvas::SDFShape::flat_segment, Canvas::SDFShape::rounded_segment,
        Canvas::SDFShape::stadium,
    };
    for (const auto& name : chart_shader_preset_names()) {
        for (auto profile : {ChartProfile::round, ChartProfile::flat, ChartProfile::chamfer}) {
            ChartShaderPresetOptions options;
            options.profile = profile;
            options.pressed = profile == ChartProfile::flat;
            const auto preset = make_chart_shader_preset(name, options);
            for (auto shape : shapes) {
                const auto error = Canvas::compile_sdf_chart_sksl(shape, preset.sksl);
                INFO(name << " shape " << static_cast<int>(shape) << ": " << error);
                REQUIRE(error.empty());
            }
        }
    }
#else
    SUCCEED("compiling chart presets requires PULP_HAS_SKIA");
#endif
}

TEST_CASE("lit chart presets follow the light direction", "[view][shader][chart-preset]") {
#ifdef PULP_HAS_SKIA
    if (!raw_rgba_render_available())
        SKIP("this build has no raw RGBA render backend");
    // Each lit preset is rendered with the light from above (90 degrees) and
    // from below (270). The edge facing the light must be the brighter one, so
    // flipping the light flips the sign of top-minus-bottom luminance. A
    // shader that ignored the light, or lit in the wrong frame, keeps its sign.
    struct Case {
        const char* name;
        bool pressed;
        ChartProfile profile;
        int k;
        float sign;
    };
    const Case cases[] = {
        // A raised bevel's top edge faces up.
        {"bevel", false, ChartProfile::round, 7, +1.0f},
        // Pressed, the same edge faces down into the plate.
        {"bevel", true, ChartProfile::round, 7, -1.0f},
        // A channel's upper wall faces down, toward its floor.
        {"inset-channel", false, ChartProfile::round, 7, -1.0f},
        {"tube", false, ChartProfile::round, 7, +1.0f},
        {"tube", false, ChartProfile::flat, 7, +1.0f},
        {"tube", false, ChartProfile::chamfer, 7, +1.0f},
        // The light pipe's emission is symmetric; its specular glint, which
        // peaks where the surface faces the half vector, is not.
        {"light-pipe", false, ChartProfile::round, 3, +1.0f},
    };
    for (const auto& c : cases) {
        INFO(c.name << " pressed=" << c.pressed << " profile=" << static_cast<int>(c.profile));
        auto from_above = lit_from(90.0f);
        from_above.pressed = c.pressed;
        from_above.profile = c.profile;
        auto from_below = from_above;
        from_below.light_angle_deg = 270.0f;
        const auto above = render_segment(c.name, from_above);
        const auto below = render_segment(c.name, from_below);
        REQUIRE(above.drawn);
        REQUIRE(below.drawn);
        const float a = above.top_minus_bottom(c.k);
        const float b = below.top_minus_bottom(c.k);
        INFO("top-bottom luma: light above " << a << ", light below " << b);
        CHECK(a * c.sign > 8.0f);
        CHECK(b * c.sign < -8.0f);
    }
#else
    SUCCEED("chart preset renders require PULP_HAS_SKIA");
#endif
}

TEST_CASE("chart preset profiles and unlit presets shade their cross-section",
          "[view][shader][chart-preset]") {
#ifdef PULP_HAS_SKIA
    if (!raw_rgba_render_available())
        SKIP("this build has no raw RGBA render backend");
    // A flat face facing the viewer reads exactly the authored colour, so a
    // flat or chamfered tube is level across its middle while a round one
    // already turns toward the light there.
    for (auto profile : {ChartProfile::round, ChartProfile::flat, ChartProfile::chamfer}) {
        auto options = lit_from(90.0f);
        options.profile = profile;
        const auto tube = render_segment("tube", options);
        const float mid = tube.top_minus_bottom(4); // |u| = 0.5
        INFO("profile " << static_cast<int>(profile) << " top-bottom at |u|=0.5: " << mid);
        if (profile == ChartProfile::round)
            CHECK(mid > 8.0f);
        else
            CHECK(std::abs(mid) < 1.5f);
    }
    ChartShaderPresetOptions flat_face = lit_from(90.0f);
    flat_face.profile = ChartProfile::flat;
    flat_face.intensity = 0.0f; // no specular
    flat_face.color = pulp::canvas::Color::rgba8(150, 155, 165);
    const auto plate = render_segment("tube", flat_face);
    const auto* centre = &plate.rgba[(16u * 128u + 64u) * 4u];
    CHECK(std::abs(int(centre[0]) - 150) <= 1);
    CHECK(std::abs(int(centre[1]) - 155) <= 1);
    CHECK(std::abs(int(centre[2]) - 165) <= 1);

    // Lighting happens in linear light. The raised bevel's top edge (slope
    // 1.2 toward the light) brightens a grey by the normalised Lambert factor;
    // applied to sRGB-decoded values it lands well below the same factor
    // applied to the encoded byte.
    ChartShaderPresetOptions grey = lit_from(90.0f);
    grey.color = pulp::canvas::Color::rgba8(150, 150, 150);
    const auto bevel = render_segment("bevel", grey);
    const float n_len = std::sqrt(1.2f * 1.2f + 1.0f);
    const float l = 0.70710678f;
    const float lambert = (0.35f + 0.65f * ((1.2f * l + l) / n_len)) / (0.35f + 0.65f * l);
    auto decode = [](float c) {
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    auto encode = [](float c) {
        c = std::min(std::max(c, 0.0f), 1.0f);
        return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    };
    const float in_linear = 255.0f * encode(decode(150.0f / 255.0f) * lambert);
    const float in_srgb = std::min(255.0f, 150.0f * lambert);
    const float edge = bevel.rgba[(9u * 128u + 64u) * 4u];
    INFO("bevel top edge " << edge << ", linear model " << in_linear << ", sRGB model " << in_srgb);
    CHECK(std::abs(edge - in_linear) <= 2.0f);
    CHECK(std::abs(edge - in_srgb) > 10.0f);

    // Centre highlight: brighter on the centre line than near the edges, and
    // level when its intensity is zero.
    ChartShaderPresetOptions highlight;
    highlight.color = pulp::canvas::Color::rgba8(40, 40, 48);
    const auto lifted = render_segment("centre-highlight", highlight);
    CHECK(lifted.luma(64, 16) - lifted.luma(64, 9) > 40.0f);
    highlight.intensity = 0.0f;
    const auto level = render_segment("centre-highlight", highlight);
    CHECK(std::abs(level.luma(64, 16) - level.luma(64, 9)) < 1.5f);

    // Glow: light 3 units outside the band's top edge (row 8), and none there
    // when sigma is zero.
    ChartShaderPresetOptions glow;
    glow.glow_sigma = 4.0f;
    const auto glowing = render_segment("glow", glow);
    glow.glow_sigma = 0.0f;
    const auto sharp = render_segment("glow", glow);
    const auto background = render_segment("glow", glow).luma(64, 1);
    INFO("glow luma at row 5: " << glowing.luma(64, 5) << " sharp: " << sharp.luma(64, 5)
                                << " background: " << background);
    CHECK(glowing.luma(64, 5) > background + 30.0f);
    CHECK(std::abs(sharp.luma(64, 5) - background) < 1.5f);
#else
    SUCCEED("chart preset renders require PULP_HAS_SKIA");
#endif
}

TEST_CASE("chart shader preset literals ignore a comma-decimal locale",
          "[view][shader][chart-preset][locale]") {
    // A host that called setlocale with a comma-decimal locale must not turn
    // `0.65` into `0,65`, which SkSL reads as two arguments.
    const char* previous = std::setlocale(LC_NUMERIC, nullptr);
    const std::string saved = previous ? previous : "C";
    if (!std::setlocale(LC_NUMERIC, "de_DE.UTF-8"))
        SKIP("the de_DE.UTF-8 locale is not installed");
    char probe[16];
    std::snprintf(probe, sizeof(probe), "%.1f", 0.5);
    const bool comma_locale = std::string(probe) == "0,5";
    ChartShaderPresetOptions options;
    options.light_angle_deg = 123.4f;
    const auto preset = make_chart_shader_preset("tube", options);
    std::string compile_error;
#ifdef PULP_HAS_SKIA
    compile_error = Canvas::compile_sdf_chart_sksl(Canvas::SDFShape::flat_arc, preset.sksl);
#endif
    std::setlocale(LC_NUMERIC, saved.c_str());
    REQUIRE(comma_locale); // the locale really is in effect, or the test proves nothing
    REQUIRE(preset.error.empty());
    REQUIRE_FALSE(std::regex_search(preset.sksl, std::regex("[0-9],[0-9]")));
    INFO(compile_error);
    REQUIRE(compile_error.empty());
}

TEST_CASE("lit presets shade a rounded cap as a dome without a seam",
          "[view][shader][chart-preset]") {
#ifdef PULP_HAS_SKIA
    if (!raw_rgba_render_available())
        SKIP("this build has no raw RGBA render backend");
    // A 128 x 32 stadium: radius 14, the right cap's centre at x = 112 and its
    // tip at 126. Just inside the tip, one row either side of the axis (row
    // 16), neighbouring rows face almost the same way, so luma must change in
    // small steps. A cap whose cross flipped sign across the axis faced rows
    // above and below it opposite ways and drew a hard line there.
    ChartShaderPresetOptions options = lit_from(90.0f);
    for (const char* name : {"tube", "bevel"}) {
        const auto cap = render_segment(name, options, Canvas::SDFShape::stadium);
        REQUIRE(cap.drawn);
        float largest_step = 0.0f;
        for (int row = 13; row < 19; ++row)
            largest_step =
                std::max(largest_step, std::abs(cap.luma(122, row + 1) - cap.luma(122, row)));
        INFO(name << " largest row-to-row luma step across the axis in the cap: " << largest_step);
        CHECK(largest_step < 10.0f);
    }
#else
    SUCCEED("chart preset renders require PULP_HAS_SKIA");
#endif
}

TEST_CASE("a knob painted by a chart preset alone still shows its value",
          "[view][shader][chart-preset]") {
#ifdef PULP_HAS_SKIA
    if (!raw_rgba_render_available())
        SKIP("this build has no raw RGBA render backend");
    // The chart is the full arc at every value, so the knob draws its value
    // pointer over it. Two values must render differently, one value the same.
    auto render_knob = [](float value) {
        Knob knob;
        knob.set_bounds({0, 0, 80, 80});
        knob.set_value(value);
        auto preset = make_chart_shader_preset("bevel");
        REQUIRE(preset.error.empty());
        knob.set_chart_shader(std::move(preset.sksl), preset.reach);
        uint32_t w = 0, h = 0;
        auto rgba = render_to_rgba(knob, 80, 80, 1.0f, &w, &h);
        REQUIRE(w == 80);
        return rgba;
    };
    auto differing = [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
        REQUIRE(a.size() == b.size());
        int count = 0;
        for (size_t i = 0; i < a.size(); i += 4)
            if (std::abs(int(a[i]) - int(b[i])) + std::abs(int(a[i + 1]) - int(b[i + 1])) +
                    std::abs(int(a[i + 2]) - int(b[i + 2])) >
                24)
                ++count;
        return count;
    };
    const auto low = render_knob(0.2f);
    const auto high = render_knob(0.8f);
    const auto low_again = render_knob(0.2f);
    INFO("pixels differing between 0.2 and 0.8: " << differing(low, high));
    CHECK(differing(low, high) > 40);
    CHECK(differing(low, low_again) == 0);
#else
    SUCCEED("chart preset renders require PULP_HAS_SKIA");
#endif
}
