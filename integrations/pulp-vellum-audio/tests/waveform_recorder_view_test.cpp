#include <pulp/vellum_audio/waveform_recorder_view.hpp>

#include <vellum/view/screenshot.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

using pulp::vellum_audio::WaveformRecorderView;

TEST_CASE("WaveformRecorderView decodes its embedded design to a sized panel",
          "[pulp-vellum-audio][waveform-recorder-view]") {
    WaveformRecorderView view;
    // The embedded vector export is 1400 x 1191; an empty decode would leave the
    // panel unsized.
    CHECK(view.panel_width() == 1400.0f);
    CHECK(view.panel_height() == 1191.0f);
    CHECK(view.intrinsic_width() == 1400.0f);
    CHECK(view.intrinsic_height() == 1191.0f);
}

TEST_CASE("WaveformRecorderView paints the design, not an empty panel",
          "[pulp-vellum-audio][waveform-recorder-view][render]") {
    if (!::vellum::view::raw_rgba_render_available())
        SKIP("Vellum CPU raster backend unavailable");

    WaveformRecorderView view;
    view.set_bounds({0, 0, 700, 596});
    std::uint32_t w = 0, h = 0;
    const auto px = ::vellum::view::render_to_rgba(view, 700, 596, 1.0f, &w, &h);
    REQUIRE(w == 700);
    REQUIRE(h == 596);

    // The design draws on a dark ground with lighter chrome and a teal accent:
    // count distinct coarse colours so a blank or single-colour render fails.
    std::vector<std::uint32_t> seen;
    for (std::size_t i = 0; i + 3 < px.size(); i += 4 * 97) {
        const std::uint32_t key = (px[i] >> 5) << 6 | (px[i + 1] >> 5) << 3 | (px[i + 2] >> 5);
        bool known = false;
        for (auto s : seen)
            known = known || s == key;
        if (!known)
            seen.push_back(key);
    }
    CHECK(seen.size() >= 6);

    if (const char* dir = std::getenv("PULP_VELLUM_AUDIO_PNG_DIR"))
        CHECK(::vellum::view::render_to_file(view, 700, 596,
                                             std::string(dir) + "/waveform-recorder-view.png", 1.0f,
                                             ::vellum::view::ScreenshotBackend::skia));
}
