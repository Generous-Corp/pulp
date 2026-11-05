// Acceptance item 3c: the RmlUi knob document drawn by Vellum's own renderer
// (VellumRmlUi::VellumRenderer on an offscreen SkiaDawnSurface) matches the
// soft renderer's frame, which is itself the module's golden image.
//
// The renderers rasterise differently (GPU coverage and colour arithmetic), so
// the comparison is a stated tolerance, not identity: a pixel differs when any
// channel differs by more than kChannelTolerance, and at most
// kMaxDifferingPercent of the pixels may differ. The empty document and the
// knob at a different gain must fail the same rule, so the comparison is shown
// able to reject a wrong frame.
//
// Built only when the installed Vellum SDK carries the VellumRenderer
// component. It links Vellum and nothing from Pulp's tree.

#include <vellum/graphics/dawn_native_bootstrap.hpp>
#include <vellum/graphics/skia_dawn_surface.hpp>
#include <vellum/rmlui/backend.hpp>
#include <vellum/rmlui/soft_renderer.hpp>
#include <vellum/rmlui/vellum_renderer.hpp>

#include "slice_rmlui_document.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace rml = vellum::rmlui;

namespace {

constexpr std::uint32_t kWidth = 160;
constexpr std::uint32_t kHeight = 96;
constexpr int kChannelTolerance = 16;
constexpr double kMaxDifferingPercent = 1.0;

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "[rmlui-renderer] FAIL: %s\n", what);
    } else {
        std::printf("[rmlui-renderer] ok: %s\n", what);
    }
}

class DocumentAssets final : public rml::AssetProvider {
  public:
    rml::Status load(std::string_view path, rml::Asset& out) override {
        if (path == "knob_led.rml")
            out.bytes = kSliceRmlDocument;
        else if (path == "empty.rml")
            out.bytes = kSliceEmptyRmlDocument;
        else
            return rml::Status::missing_asset;
        out.path = std::string(path);
        return rml::Status::ok;
    }
};

class GainModel final : public rml::ModelHost {
  public:
    explicit GainModel(double gain) : gain_(gain) {}
    rml::Status read(std::string_view field, rml::ModelValue& out) override {
        if (field != "gain")
            return rml::Status::rejected;
        out = rml::ModelValue::number_value(gain_);
        return rml::Status::ok;
    }
    rml::Status write(std::string_view, const rml::ModelValue&) override {
        return rml::Status::rejected;
    }

  private:
    double gain_;
};

// Loads `document` with the gain model and renders it twice: the first update
// evaluates the bindings, the second lays out what they produced.
bool render(rml::RenderInterface& renderer, const char* document, double gain) {
    DocumentAssets assets;
    rml::HeadlessRecorder no_lua;
    rml::Backend backend(assets, renderer, no_lua);
    if (backend.open(kWidth, kHeight) != rml::Status::ok)
        return false;
    const bool loaded = backend.bind_model("controls", std::make_shared<GainModel>(gain),
                                           {"gain"}) == rml::Status::ok &&
                        backend.load_document(document) == rml::Status::ok;
    if (loaded) {
        backend.render();
        backend.render();
    }
    backend.close();
    return loaded;
}

struct Comparison {
    int max_delta = 0;
    std::size_t differing = 0;
    double differing_percent = 100.0;
    bool within_tolerance = false;
};

Comparison compare(const std::vector<std::uint8_t>& actual,
                   const std::vector<std::uint8_t>& expected) {
    Comparison result;
    if (actual.size() != expected.size() || actual.empty())
        return result;
    for (std::size_t i = 0; i + 3 < actual.size(); i += 4) {
        int delta = 0;
        for (int k = 0; k < 4; ++k)
            delta = std::max(delta, std::abs(int(actual[i + k]) - int(expected[i + k])));
        result.max_delta = std::max(result.max_delta, delta);
        if (delta > kChannelTolerance)
            ++result.differing;
    }
    result.differing_percent =
        100.0 * static_cast<double>(result.differing) / static_cast<double>(actual.size() / 4);
    result.within_tolerance = result.differing_percent <= kMaxDifferingPercent;
    return result;
}

void report(const char* label, const Comparison& c) {
    std::printf("[rmlui-renderer] %s: max channel delta %d, %zu pixels (%.3f%%) differ by more "
                "than %d\n",
                label, c.max_delta, c.differing, c.differing_percent, kChannelTolerance);
}

// One frame through VellumRenderer; empty on failure.
std::vector<std::uint8_t> render_vellum(vellum::graphics::SkiaDawnSurface& surface,
                                        const char* document, double gain, bool& frame_ok,
                                        std::size_t& triangles, std::size_t& skipped) {
    rml::VellumRenderer renderer(surface, 0x000000ffu);
    const bool rendered = render(renderer, document, gain);
    frame_ok = rendered && renderer.frame_ok();
    if (!renderer.last_error().empty())
        std::fprintf(stderr, "[rmlui-renderer] %s\n", renderer.last_error().c_str());
    triangles = renderer.triangles_submitted();
    skipped = renderer.skipped_draws();
    std::vector<std::uint8_t> rgba;
    std::uint32_t w = 0, h = 0;
    std::string error;
    if (!surface.capture_rgba(rgba, w, h, &error) || w != kWidth || h != kHeight) {
        std::fprintf(stderr, "[rmlui-renderer] capture failed: %s\n", error.c_str());
        return {};
    }
    return rgba;
}

std::string read_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

int main() {
    std::string error;
    if (!vellum::app_host::register_native_dawn_bootstrap(&error)) {
        std::fprintf(stderr, "[rmlui-renderer] FAIL: Dawn bootstrap: %s\n", error.c_str());
        return 1;
    }
    auto surface = vellum::graphics::SkiaDawnSurface::create(
        {.width = kWidth, .height = kHeight, .scale = 1.0f, .native_surface_handle = nullptr},
        &error);
    check(surface != nullptr, "an offscreen SkiaDawnSurface opens");
    if (!surface) {
        std::fprintf(stderr, "[rmlui-renderer] SkiaDawnSurface: %s\n", error.c_str());
        return 1;
    }

    // The reference: the soft renderer's frame, which is the golden image.
    rml::SoftRenderer soft(0x000000ffu);
    check(render(soft, "knob_led.rml", 0.75) && soft.skipped_draws() == 0,
          "the soft renderer renders the knob document");
    const auto encoded = soft.encode_png();
    check(std::string(encoded.begin(), encoded.end()) == read_file(SLICE_GOLDEN_PNG),
          "the soft renderer's frame equals the module's golden image");
    const auto& reference = soft.pixels();

    bool frame_ok = false;
    std::size_t triangles = 0, skipped = 0;
    const auto knob = render_vellum(*surface, "knob_led.rml", 0.75, frame_ok, triangles, skipped);
    check(frame_ok && triangles > 0 && skipped == 0 && !knob.empty(),
          "the Vellum renderer renders the knob document");
    const auto knob_cmp = compare(knob, reference);
    report("knob_led.rml at gain 0.75", knob_cmp);
    check(knob_cmp.within_tolerance,
          "the Vellum renderer's frame is within the tolerance of the soft renderer's");

    // Negative controls: the same comparison must reject a wrong frame.
    const auto empty = render_vellum(*surface, "empty.rml", 0.75, frame_ok, triangles, skipped);
    const auto empty_cmp = compare(empty, reference);
    report("empty.rml (negative control)", empty_cmp);
    check(frame_ok && !empty.empty() && !empty_cmp.within_tolerance,
          "an empty document falls outside that tolerance");
    const auto low = render_vellum(*surface, "knob_led.rml", 0.2, frame_ok, triangles, skipped);
    const auto low_cmp = compare(low, reference);
    report("knob_led.rml at gain 0.2 (negative control)", low_cmp);
    check(frame_ok && !low.empty() && !low_cmp.within_tolerance,
          "the knob at gain 0.2 falls outside that tolerance");

    std::printf("[rmlui-renderer] %s\n", g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
