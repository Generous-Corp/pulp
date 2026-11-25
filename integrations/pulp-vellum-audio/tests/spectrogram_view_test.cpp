#include <pulp/vellum_audio/spectrogram_view.hpp>

#include <vellum/canvas/recording_canvas.hpp>
#include <vellum/view/screenshot.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

using Catch::Approx;
using pulp::signal::ColorRamp;
using pulp::vellum_audio::SpectrogramView;
using ::vellum::canvas::DrawCommand;
using ::vellum::canvas::RecordingCanvas;

namespace {

struct Frame {
    std::vector<std::uint8_t> pixels;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    int r(std::uint32_t x, std::uint32_t y) const {
        return pixels[(static_cast<std::size_t>(y) * width + x) * 4];
    }
};

Frame render(::vellum::view::View& view, std::uint32_t w, std::uint32_t h) {
    Frame frame;
    frame.pixels = ::vellum::view::render_to_rgba(view, w, h, 1.0f, &frame.width, &frame.height);
    return frame;
}

// PULP_VELLUM_AUDIO_PNG_DIR names a directory to receive the rendered frames
// for review; unset (the default) writes nothing.
void maybe_write_png(::vellum::view::View& view, std::uint32_t w, std::uint32_t h,
                     const std::string& name) {
    if (const char* dir = std::getenv("PULP_VELLUM_AUDIO_PNG_DIR"))
        CHECK(::vellum::view::render_to_file(view, w, h, std::string(dir) + "/" + name, 4.0f));
}

} // namespace

TEST_CASE("Vellum SpectrogramView configure and push", "[pulp-vellum-audio][spectrogram]") {
    SpectrogramView spectrogram;
    spectrogram.configure(128, 64, ColorRamp::inferno, -80.0f, 0.0f);
    REQUIRE(spectrogram.history_columns() == 128);
    REQUIRE(spectrogram.freq_rows() == 64);

    std::vector<float> mags(256, -40.0f);
    spectrogram.push_spectrum(mags.data(), 256);
    CHECK(spectrogram.buffer().frames_written() == 1);
}

TEST_CASE("Vellum SpectrogramView auto-configures on first push",
          "[pulp-vellum-audio][spectrogram]") {
    SpectrogramView spectrogram;
    std::vector<float> mags(128, -20.0f);
    spectrogram.push_spectrum(mags.data(), 128);
    REQUIRE(spectrogram.freq_rows() == 128);
    REQUIRE(spectrogram.history_columns() == 256);
}

TEST_CASE("Vellum SpectrogramView paints one cell per history column and row",
          "[pulp-vellum-audio][spectrogram]") {
    SpectrogramView spectrogram;
    spectrogram.set_bounds({0, 0, 40, 20});

    RecordingCanvas empty;
    spectrogram.paint(empty);
    REQUIRE(empty.count(DrawCommand::Type::fill_rect) == 1); // background only

    spectrogram.configure(4, 2, ColorRamp::heat, -60.0f, -20.0f);
    spectrogram.set_color_ramp(ColorRamp::grayscale);
    spectrogram.set_range(-90.0f, -30.0f);
    std::vector<float> magnitudes{-90.0f, -30.0f};
    spectrogram.push_spectrum(magnitudes.data(), 2);

    RecordingCanvas canvas;
    spectrogram.paint(canvas);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) == 1 + 4 * 2);
}

TEST_CASE("Vellum SpectrogramView fills cells with unit-range colours",
          "[pulp-vellum-audio][spectrogram]") {
    SpectrogramView spectrogram;
    spectrogram.set_bounds({0, 0, 8, 4});
    spectrogram.configure(2, 1, ColorRamp::grayscale, -60.0f, -20.0f);
    const float mid = -40.0f; // normalised 0.5
    spectrogram.push_spectrum(&mid, 1);

    RecordingCanvas canvas;
    spectrogram.paint(canvas);
    bool saw_mid_grey = false;
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type != DrawCommand::Type::set_fill_color)
            continue;
        CHECK(cmd.color.r <= 1.0f);
        CHECK(cmd.color.g <= 1.0f);
        CHECK(cmd.color.b <= 1.0f);
        CHECK(cmd.color.a <= 1.0f);
        saw_mid_grey = saw_mid_grey || cmd.color.r == Approx(127.0f / 255.0f).margin(1e-3);
    }
    CHECK(saw_mid_grey);
}

TEST_CASE("Vellum SpectrogramView mutators request repaint", "[pulp-vellum-audio][spectrogram]") {
    SpectrogramView spectrogram;
    auto before = ::vellum::view::View::damage_request_count();
    spectrogram.configure(4, 2);
    CHECK(::vellum::view::View::damage_request_count() > before);

    const float bin = -40.0f;
    before = ::vellum::view::View::damage_request_count();
    spectrogram.push_spectrum(&bin, 1);
    CHECK(::vellum::view::View::damage_request_count() > before);

    before = ::vellum::view::View::damage_request_count();
    spectrogram.set_color_ramp(ColorRamp::viridis);
    CHECK(::vellum::view::View::damage_request_count() > before);

    before = ::vellum::view::View::damage_request_count();
    spectrogram.set_range(-60.0f, 0.0f);
    CHECK(::vellum::view::View::damage_request_count() > before);
}

TEST_CASE("Vellum SpectrogramView renders mid-level cells as mid grey",
          "[pulp-vellum-audio][spectrogram][render]") {
    if (!::vellum::view::raw_rgba_render_available())
        SKIP("Vellum CPU raster backend unavailable");

    // One row, four columns: oldest to newest are black, mid grey, white, with
    // the freshest column on the right. The mid cell is where an 8-bit value
    // written into a float channel would saturate to white.
    SpectrogramView spectrogram;
    spectrogram.set_bounds({0, 0, 40, 20});
    spectrogram.configure(4, 1, ColorRamp::grayscale, -60.0f, -20.0f);
    for (float db : {-60.0f, -40.0f, -20.0f})
        spectrogram.push_spectrum(&db, 1);

    const auto frame = render(spectrogram, 40, 20);
    REQUIRE(frame.width == 40);
    // Columns drawn oldest first: the unwritten slot (black), then the pushes.
    CHECK(frame.r(5, 10) == 0);
    CHECK(frame.r(15, 10) == 0);
    CHECK(frame.r(25, 10) == Approx(127).margin(3));
    CHECK(frame.r(35, 10) == 255);
    maybe_write_png(spectrogram, 40, 20, "spectrogram_grayscale.png");
}

TEST_CASE("Vellum SpectrogramView draws low frequencies at the bottom",
          "[pulp-vellum-audio][spectrogram][render]") {
    if (!::vellum::view::raw_rgba_render_available())
        SKIP("Vellum CPU raster backend unavailable");

    SpectrogramView spectrogram;
    spectrogram.set_bounds({0, 0, 20, 40});
    spectrogram.configure(2, 2, ColorRamp::grayscale, -60.0f, -20.0f);
    const float bins[2] = {-60.0f, -20.0f}; // low bin dark, high bin bright
    spectrogram.push_spectrum(bins, 2);
    spectrogram.push_spectrum(bins, 2);

    const auto frame = render(spectrogram, 20, 40);
    CHECK(frame.r(10, 30) == 0);   // low bin, bottom half
    CHECK(frame.r(10, 10) == 255); // high bin, top half
}

TEST_CASE("Vellum SpectrogramView renders a ramped scroll for review",
          "[pulp-vellum-audio][spectrogram][render]") {
    if (!::vellum::view::raw_rgba_render_available())
        SKIP("Vellum CPU raster backend unavailable");

    SpectrogramView spectrogram;
    spectrogram.set_bounds({0, 0, 160, 80});
    spectrogram.configure(32, 16, ColorRamp::inferno, -80.0f, 0.0f);
    std::vector<float> column(16);
    for (int c = 0; c < 40; ++c) {
        for (int bin = 0; bin < 16; ++bin)
            column[static_cast<std::size_t>(bin)] =
                -80.0f + 80.0f * static_cast<float>((bin * 2 + c) % 32) / 32.0f;
        spectrogram.push_spectrum(column.data(), 16);
    }
    const auto frame = render(spectrogram, 160, 80);
    REQUIRE(frame.width == 160);
    maybe_write_png(spectrogram, 160, 80, "spectrogram_inferno.png");
}
