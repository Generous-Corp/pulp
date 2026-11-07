// test_audio_image_raster.cpp — unit tests for the spectrogram and waveform
// image pipelines in tools/audio/analysis.
//
// Every stage is exercised on its own (palette, mel mapping, row/bin bands,
// ticks, font, STFT grid, column reduction) and then end to end against
// small golden PNGs under test/fixtures/audio/images/.
//
// Golden regeneration: build this target with pulp::render available, then
//     PULP_AUDIO_IMAGE_WRITE_GOLDENS=1 ./build/test/pulp-test-audio-image-raster "[golden]"
// writes the fixtures and the test passes trivially for that run. Commit the
// PNGs with the change that moved them and say why in the commit message.
// Comparison decodes through ImageIO, so the golden cases run on macOS (the
// required gate) and skip elsewhere; every other case runs everywhere.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/analysis/raster_image.hpp>
#include <pulp/audio/analysis/spectrogram_raster.hpp>
#include <pulp/audio/analysis/waveform_raster.hpp>

#if defined(PULP_TEST_HAS_RENDER)
#include <pulp/render/headless_surface.hpp>
#endif

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <string>
#include <vector>

#if defined(__APPLE__)
std::vector<uint8_t> decode_png_rgba(const std::vector<uint8_t>& png, size_t& w, size_t& h);
#endif

using namespace pulp::test::audio;
using Catch::Approx;
namespace fs = std::filesystem;

namespace {

std::vector<float> sine(double hz, double amplitude, double sample_rate, std::size_t frames) {
    std::vector<float> out(frames);
    for (std::size_t i = 0; i < frames; ++i)
        out[i] = static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * hz *
                                                         static_cast<double>(i) / sample_rate));
    return out;
}

// Linear chirp from f0 to f1 over the buffer.
std::vector<float> chirp(double f0, double f1, double amplitude, double sample_rate,
                         std::size_t frames) {
    std::vector<float> out(frames);
    const double duration = static_cast<double>(frames) / sample_rate;
    for (std::size_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / sample_rate;
        const double phase =
            2.0 * std::numbers::pi * (f0 * t + (f1 - f0) * t * t / (2.0 * duration));
        out[i] = static_cast<float>(amplitude * std::sin(phase));
    }
    return out;
}

double luminance(Rgb8 c) {
    return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
}

fs::path golden_dir() {
    return fs::path(PULP_SOURCE_DIR) / "test/fixtures/audio/images";
}

std::vector<uint8_t> read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Compares a rendered image with its golden. Tolerance: no channel of any
// pixel moves by more than 8/255 and at most 1 % of pixels differ at all,
// which absorbs a one-step palette shift from FFT rounding but not a moved
// axis, a missing marker or a changed glyph.
void check_against_golden(const RasterImage& image, const std::string& name) {
    const fs::path path = golden_dir() / name;
    if (std::getenv("PULP_AUDIO_IMAGE_WRITE_GOLDENS") != nullptr) {
#if defined(PULP_TEST_HAS_RENDER)
        pulp::render::HeadlessSurface::Rgba rgba;
        rgba.width = image.width;
        rgba.height = image.height;
        rgba.pixels = image.rgba;
        std::string error;
        const auto png = pulp::render::HeadlessSurface::encode_png(rgba, &error);
        REQUIRE_FALSE(png.empty());
        fs::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(png.data()),
                  static_cast<std::streamsize>(png.size()));
        REQUIRE(out.good());
        SUCCEED("wrote golden " + path.string());
        return;
#else
        FAIL("golden regeneration needs a build with pulp::render");
#endif
    }
#if defined(__APPLE__)
    REQUIRE(fs::exists(path));
    size_t w = 0;
    size_t h = 0;
    const auto golden = decode_png_rgba(read_bytes(path), w, h);
    REQUIRE(w == image.width);
    REQUIRE(h == image.height);
    REQUIRE(golden.size() == image.rgba.size());
    std::size_t differing_pixels = 0;
    int max_delta = 0;
    for (std::size_t i = 0; i < golden.size(); i += 4) {
        bool differs = false;
        for (std::size_t c = 0; c < 3; ++c) {
            const int delta =
                std::abs(static_cast<int>(golden[i + c]) - static_cast<int>(image.rgba[i + c]));
            max_delta = std::max(max_delta, delta);
            differs = differs || delta != 0;
        }
        if (differs)
            ++differing_pixels;
    }
    INFO("golden " << name << " max channel delta " << max_delta << ", differing pixels "
                   << differing_pixels << " of " << image.rgba.size() / 4);
    CHECK(max_delta <= 8);
    CHECK(differing_pixels * 100 <= image.rgba.size() / 4);
#else
    SKIP("golden PNG comparison decodes through ImageIO and runs on macOS");
#endif
}

} // namespace

// ── Palette ────────────────────────────────────────────────────────────────

TEST_CASE("spectrogram palette is clamped, deterministic and monotone in luminance",
          "[audio][image][palette]") {
    CHECK(spectrogram_palette(-1.0) == spectrogram_palette(0.0));
    CHECK(spectrogram_palette(2.0) == spectrogram_palette(1.0));
    CHECK(spectrogram_palette(0.5) == spectrogram_palette(0.5));
    double previous = -1.0;
    for (int step = 0; step <= 100; ++step) {
        const double lum = luminance(spectrogram_palette(step / 100.0));
        CHECK(lum >= previous - 1e-9);
        previous = lum;
    }
    CHECK(luminance(spectrogram_palette(1.0)) > luminance(spectrogram_palette(0.0)) + 150.0);
}

TEST_CASE("db_to_palette_unit maps the display window onto [0, 1]", "[audio][image][palette]") {
    CHECK(db_to_palette_unit(0.0, 0.0, 90.0) == Approx(1.0));
    CHECK(db_to_palette_unit(-90.0, 0.0, 90.0) == Approx(0.0));
    CHECK(db_to_palette_unit(-45.0, 0.0, 90.0) == Approx(0.5));
    CHECK(db_to_palette_unit(+12.0, 0.0, 90.0) == Approx(1.0));  // above ceiling clamps
    CHECK(db_to_palette_unit(-200.0, 0.0, 90.0) == Approx(0.0)); // below floor clamps
    CHECK(db_to_palette_unit(-10.0, 0.0, 0.0) == Approx(0.0));   // degenerate range
}

// ── Frequency axis ─────────────────────────────────────────────────────────

TEST_CASE("mel mapping follows the HTK convention and inverts exactly", "[audio][image][mel]") {
    CHECK(hz_to_mel(0.0) == Approx(0.0));
    CHECK(hz_to_mel(1000.0) == Approx(1000.0).epsilon(1e-3));
    for (double hz : {20.0, 440.0, 1000.0, 4000.0, 12000.0, 24000.0})
        CHECK(mel_to_hz(hz_to_mel(hz)) == Approx(hz).epsilon(1e-9));
    CHECK(parse_frequency_scale("mel") == FrequencyScale::mel);
    CHECK(parse_frequency_scale("log") == FrequencyScale::log);
    CHECK(parse_frequency_scale("linear") == FrequencyScale::linear);
    CHECK_FALSE(parse_frequency_scale("bark").has_value());
    CHECK(frequency_scale_name(FrequencyScale::mel) == "mel");
}

TEST_CASE("frequency_to_unit is monotone and spans [0, 1] on every scale", "[audio][image][axis]") {
    for (auto scale : {FrequencyScale::linear, FrequencyScale::log, FrequencyScale::mel}) {
        const double lo = axis_low_frequency(48000.0, 2048, scale);
        const double hi = 24000.0;
        CHECK(frequency_to_unit(lo, lo, hi, scale) == Approx(0.0).margin(1e-12));
        CHECK(frequency_to_unit(hi, lo, hi, scale) == Approx(1.0).margin(1e-12));
        double previous = -1.0;
        for (double hz = std::max(lo, 1.0); hz <= hi; hz *= 1.3) {
            const double unit = frequency_to_unit(hz, lo, hi, scale);
            CHECK(unit > previous);
            CHECK(unit_to_frequency(unit, lo, hi, scale) == Approx(hz).epsilon(1e-9));
            previous = unit;
        }
    }
    CHECK(axis_low_frequency(48000.0, 2048, FrequencyScale::log) == Approx(48000.0 / 2048.0));
    CHECK(axis_low_frequency(48000.0, 2048, FrequencyScale::linear) == 0.0);
}

TEST_CASE("row_bin_bands covers every bin exactly once in order", "[audio][image][axis]") {
    for (auto scale : {FrequencyScale::linear, FrequencyScale::log, FrequencyScale::mel}) {
        for (std::uint32_t rows : {1u, 3u, 64u, 512u}) {
            for (double sample_rate : {8000.0, 48000.0}) {
                const int fft = 1024;
                const std::uint32_t bins = fft / 2 + 1;
                const auto bands = row_bin_bands(bins, sample_rate, fft, rows, scale);
                INFO("scale " << frequency_scale_name(scale) << " rows " << rows << " sr "
                              << sample_rate);
                REQUIRE(bands.size() == rows);
                const std::uint32_t first_bin = scale == FrequencyScale::log ? 1u : 0u;
                CHECK(bands.front().last == bins - 1);
                CHECK(bands.back().first == first_bin);
                std::vector<int> coverage(bins, 0);
                std::uint32_t previous_first = bins;
                for (const auto& band : bands) {
                    CHECK(band.first <= band.last);
                    CHECK(band.last <= bins - 1);
                    // Rows run top to bottom, so bands move toward lower bins.
                    CHECK(band.first <= previous_first);
                    previous_first = band.first;
                    for (std::uint32_t bin = band.first; bin <= band.last; ++bin)
                        ++coverage[bin];
                }
                for (std::uint32_t bin = first_bin; bin < bins; ++bin) {
                    INFO("bin " << bin);
                    CHECK(coverage[bin] >= 1);
                }
            }
        }
    }
}

TEST_CASE("frequency axis labels land inside the plot", "[audio][image][axis]") {
    for (auto scale : {FrequencyScale::linear, FrequencyScale::log, FrequencyScale::mel}) {
        const auto labels = frequency_axis_labels(48000.0, 2048, scale, 256);
        REQUIRE_FALSE(labels.empty());
        for (const auto& label : labels) {
            CHECK(label.unit >= -1e-9);
            CHECK(label.unit <= 1.0 + 1e-9);
            CHECK_FALSE(label.label.empty());
        }
    }
    const auto mel = frequency_axis_labels(8000.0, 512, FrequencyScale::mel, 128);
    for (const auto& label : mel)
        CHECK(label.unit <= 1.0 + 1e-9);
}

// ── Ticks, labels, font ────────────────────────────────────────────────────

TEST_CASE("nice tick steps and time ticks", "[audio][image][ticks]") {
    CHECK(nice_tick_step(0.0, 2.0, 8) == Approx(0.5));
    CHECK(nice_tick_step(0.0, 10.0, 5) == Approx(2.0));
    CHECK(nice_tick_step(0.0, 0.0, 5) == 0.0);
    const auto ticks = time_axis_ticks(0.0, 2.0, 8);
    REQUIRE(ticks.size() == 5);
    CHECK(ticks[0].label == "0");
    CHECK(ticks[1].label == "0.5");
    CHECK(ticks[2].label == "1");
    CHECK(ticks[4].label == "2");
    const auto offset = time_axis_ticks(3600.25, 3602.0, 4);
    REQUIRE_FALSE(offset.empty());
    CHECK(offset.front().value >= 3600.25);
    CHECK(offset.back().value <= 3602.0 + 1e-9);
    CHECK(format_frequency_label(2500.0) == "2.5k");
    CHECK(format_frequency_label(20000.0) == "20k");
    CHECK(format_frequency_label(50.0) == "50");
    CHECK(format_seconds_label(1.500, 3) == "1.5");
    CHECK(format_seconds_label(0.0, 2) == "0");
}

TEST_CASE("5x7 font draws deterministic glyphs and boxes unknown bytes", "[audio][image][font]") {
    const std::uint8_t* a = glyph_rows_5x7('A');
    bool any = false;
    for (int row = 0; row < kGlyphHeight; ++row)
        any = any || a[row] != 0;
    CHECK(any);
    CHECK(glyph_rows_5x7(static_cast<char>(0x01)) == glyph_rows_5x7(static_cast<char>(0x80)));
    CHECK(text_width_5x7("abc") == 3 * kGlyphAdvance - 1);
    CHECK(text_width_5x7("") == 0);

    RasterImage image = RasterImage::filled(40, 12, Rgb8{0, 0, 0});
    draw_text_5x7(image, 1, 2, "I", Rgb8{255, 255, 255});
    // 'I' top row is 0x0E: glyph columns 1..3 lit (x = 2..4), the outer
    // columns and the row above dark.
    for (int x = 2; x <= 4; ++x)
        CHECK(image.get(x, 2) == Rgb8{255, 255, 255});
    CHECK(image.get(1, 2) == Rgb8{0, 0, 0});
    CHECK(image.get(5, 2) == Rgb8{0, 0, 0});
    CHECK(image.get(3, 1) == Rgb8{0, 0, 0});
    // Clipping at the edge does not write out of bounds or throw; 'W' lights
    // its first column.
    draw_text_5x7(image, 38, 10, "WW", Rgb8{255, 0, 0});
    CHECK(image.get(38, 10) == Rgb8{255, 0, 0});
}

TEST_CASE("chrome composition sizes the image from the plot", "[audio][image][chrome]") {
    const RasterImage plot = RasterImage::filled(100, 50, Rgb8{1, 2, 3});
    ImageChrome chrome;
    chrome.footer = "x.wav";
    chrome.start_s = 0.0;
    chrome.end_s = 1.0;
    const RasterImage image = compose_plot_with_chrome(plot, {AxisLabel{1.0, "top"}}, chrome);
    CHECK(image.width == chrome_image_width(100));
    CHECK(image.height == chrome_image_height(50));
    CHECK(image.get(kChromeLeftMargin, kChromeTopMargin) == Rgb8{1, 2, 3});
    CHECK(image.get(kChromeLeftMargin + 99, kChromeTopMargin + 49) == Rgb8{1, 2, 3});
}

// ── STFT grid ──────────────────────────────────────────────────────────────

TEST_CASE("spectrogram frame count and bin dBFS", "[audio][image][stft]") {
    CHECK(spectrogram_frame_count(1000, 512, 128) == 4);
    CHECK(spectrogram_frame_count(512, 512, 128) == 1);
    CHECK(spectrogram_frame_count(511, 512, 128) == 0);
    CHECK(spectrogram_frame_count(100, 0, 128) == 0);
    CHECK(bin_magnitude_dbfs(512.0, 1024) == Approx(0.0).margin(1e-6));
    CHECK(bin_magnitude_dbfs(0.0, 1024) == kSpectrogramFloorDb);
    CHECK(validate_spectrogram_settings({}).empty());
    SpectrogramSettings bad;
    bad.fft_length = 1000;
    CHECK(validate_spectrogram_settings(bad) == "fft length must be a power of two");
    bad = {};
    bad.hop = 0;
    CHECK(validate_spectrogram_settings(bad) == "hop must be at least 1");
    bad = {};
    bad.hop = 4096;
    CHECK_FALSE(validate_spectrogram_settings(bad).empty());
    bad = {};
    bad.db_range = -3.0;
    CHECK_FALSE(validate_spectrogram_settings(bad).empty());
}

TEST_CASE("SpectrogramBuilder reads a full-scale tone at 0 dBFS in its bin",
          "[audio][image][stft]") {
    const double sample_rate = 48000.0;
    SpectrogramSettings settings;
    settings.fft_length = 1024;
    settings.hop = 256;
    const auto samples = sine(1000.0, 1.0, sample_rate, 48000);
    SpectrogramBuilder builder(settings, sample_rate, samples.size(), 64);
    builder.push(samples);
    const SpectrogramGrid grid = builder.finish();
    REQUIRE(grid.columns == 64);
    REQUIRE(grid.bins == 513);
    CHECK(grid.frames_analyzed == spectrogram_frame_count(samples.size(), 1024, 256));
    const double bin_hz = sample_rate / 1024.0;
    const auto expected_bin = static_cast<std::uint32_t>(std::lround(1000.0 / bin_hz));
    for (std::uint32_t column : {0u, 31u, 63u}) {
        std::uint32_t peak_bin = 0;
        float peak = kSpectrogramFloorDb;
        for (std::uint32_t bin = 0; bin < grid.bins; ++bin) {
            if (grid.at(column, bin) > peak) {
                peak = grid.at(column, bin);
                peak_bin = bin;
            }
        }
        CHECK(peak_bin == expected_bin);
        // Hann scalloping at a 0.33-bin offset costs under 1 dB; nothing is normalized.
        CHECK(peak > -1.5f);
        CHECK(peak < 0.5f);
        CHECK(grid.at(column, 200) < -60.0f);
    }
}

TEST_CASE("SpectrogramBuilder is independent of how the samples are chunked",
          "[audio][image][stft]") {
    const double sample_rate = 44100.0;
    SpectrogramSettings settings;
    settings.fft_length = 512;
    settings.hop = 160;
    settings.window = Window::blackman;
    const auto samples = chirp(100.0, 15000.0, 0.7, sample_rate, 30000);

    SpectrogramBuilder whole(settings, sample_rate, samples.size(), 128);
    whole.push(samples);
    const SpectrogramGrid a = whole.finish();

    SpectrogramBuilder chunked(settings, sample_rate, samples.size(), 128);
    std::size_t offset = 0;
    std::size_t step = 1;
    while (offset < samples.size()) {
        const std::size_t count = std::min(step, samples.size() - offset);
        chunked.push(std::span<const float>(samples.data() + offset, count));
        offset += count;
        step = step * 3 + 7; // irregular block sizes straddle frame boundaries
    }
    const SpectrogramGrid b = chunked.finish();
    REQUIRE(a.magnitude_db.size() == b.magnitude_db.size());
    CHECK(a.frames_analyzed == b.frames_analyzed);
    CHECK(a.magnitude_db == b.magnitude_db);
}

TEST_CASE("SpectrogramBuilder fills columns no frame reached", "[audio][image][stft]") {
    SpectrogramSettings settings;
    settings.fft_length = 256;
    settings.hop = 256;
    const auto samples = sine(2000.0, 0.5, 16000.0, 1024); // four frames, many columns
    SpectrogramBuilder builder(settings, 16000.0, samples.size(), 40);
    builder.push(samples);
    const SpectrogramGrid grid = builder.finish();
    CHECK(grid.frames_analyzed == 4);
    for (std::uint32_t column = 0; column < grid.columns; ++column) {
        float peak = kSpectrogramFloorDb;
        for (std::uint32_t bin = 0; bin < grid.bins; ++bin)
            peak = std::max(peak, grid.at(column, bin));
        INFO("column " << column);
        CHECK(peak > -20.0f);
    }
}

TEST_CASE("spectrogram plot paints the tone row bright and the rest dark",
          "[audio][image][render]") {
    const double sample_rate = 48000.0;
    SpectrogramSettings settings;
    settings.fft_length = 512;
    settings.hop = 128;
    const auto samples = sine(1000.0, 0.5, sample_rate, 24000);
    SpectrogramBuilder builder(settings, sample_rate, samples.size(), 64);
    builder.push(samples);
    const SpectrogramGrid grid = builder.finish();
    SpectrogramImageOptions options;
    options.plot_height = 96;
    const RasterImage plot = render_spectrogram_plot(grid, options);
    REQUIRE(plot.width == 64);
    REQUIRE(plot.height == 96);
    const int tone_row = static_cast<int>(std::round((1.0 - 1000.0 / 24000.0) * (96 - 1)));
    const double bright = luminance(plot.get(32, tone_row));
    const double dark = luminance(plot.get(32, 10));
    CHECK(bright > dark + 100.0);
}

// ── Waveform ───────────────────────────────────────────────────────────────

TEST_CASE("clip detection and base resolution choice", "[audio][image][waveform]") {
    CHECK(peak_is_clipped(pulp::audio::AudioPeak::from_range(-0.2f, 1.0f)));
    CHECK(peak_is_clipped(pulp::audio::AudioPeak::from_range(-1.0f, 0.1f)));
    CHECK(
        peak_is_clipped(pulp::audio::AudioPeak::from_range(-0.5f, 1.7f))); // clamps, still clipped
    CHECK_FALSE(peak_is_clipped(pulp::audio::AudioPeak::from_range(-0.99f, 0.99f)));
    CHECK(choose_samples_per_peak(96000, 1024) == 64);
    CHECK(choose_samples_per_peak(10, 1024) == 1);
    CHECK(choose_samples_per_peak(0, 1024) == 1);
    CHECK(choose_samples_per_peak(1ull << 40, 1) == 65536);
    CHECK(choose_samples_per_peak(2048, 1) == 2048);
}

TEST_CASE("columns_from_level reduces, repeats and folds channels", "[audio][image][waveform]") {
    pulp::audio::OverviewLevel level;
    level.samples_per_peak = 4;
    level.peaks_per_channel = 8;
    level.peaks.resize(2);
    for (int p = 0; p < 8; ++p) {
        level.peaks[0].push_back(pulp::audio::AudioPeak::from_range(-0.1f * p, 0.1f * p));
        level.peaks[1].push_back(pulp::audio::AudioPeak::from_range(-0.5f, 0.5f));
    }
    level.peaks[0][7] = pulp::audio::AudioPeak::from_range(-1.0f, 1.0f);

    SECTION("reduce 8 peaks to 4 columns keeps the extremes") {
        const auto columns = columns_from_level(level, 0, 4);
        REQUIRE(columns.width() == 4);
        CHECK(columns.max[0] == Approx(0.1f).margin(0.01));
        CHECK(columns.max[3] == Approx(1.0f));
        CHECK(columns.min[3] == Approx(-1.0f));
        CHECK(columns.clipped[3] == 1);
        CHECK(columns.clipped[0] == 0);
    }
    SECTION("more columns than peaks repeats each peak") {
        const auto columns = columns_from_level(level, 1, 16);
        REQUIRE(columns.width() == 16);
        for (std::uint32_t c = 0; c < 16; ++c) {
            CHECK(columns.max[c] == Approx(0.5f).margin(0.01));
            CHECK(columns.min[c] == Approx(-0.5f).margin(0.01));
        }
    }
    SECTION("all-channels fold takes the outer envelope") {
        const auto columns =
            columns_from_level(level, pulp::audio::WaveformOverview::kAllChannels, 8);
        CHECK(columns.max[0] == Approx(0.5f).margin(0.01));
        CHECK(columns.max[7] == Approx(1.0f));
        CHECK(columns.clipped[7] == 1);
    }
    SECTION("out-of-range channel and zero width yield nothing") {
        CHECK(columns_from_level(level, 2, 8).width() == 0);
        CHECK(columns_from_level(level, 0, 0).width() == 0);
    }
}

TEST_CASE("columns_from_overview selects a level and concatenate_levels stitches chunks",
          "[audio][image][waveform]") {
    const auto samples = sine(50.0, 0.8, 8000.0, 8192);
    const float* ptr = samples.data();
    pulp::audio::BufferView<const float> view(&ptr, 1, samples.size());
    const auto overview = pulp::audio::WaveformOverview::build_from_buffer_view(view, 8000, 16);
    REQUIRE_FALSE(overview.empty());
    const auto columns = columns_from_overview(overview, 0, 32);
    REQUIRE(columns.width() == 32);
    float peak = 0.0f;
    for (float v : columns.max)
        peak = std::max(peak, v);
    CHECK(peak == Approx(0.8f).margin(0.02));

    // Two half-buffers built separately concatenate to the same base level.
    pulp::audio::BufferView<const float> first(&ptr, 1, 4096);
    const float* second_ptr = samples.data() + 4096;
    pulp::audio::BufferView<const float> second(&second_ptr, 1, 4096);
    const auto a = pulp::audio::WaveformOverview::build_from_buffer_view(first, 8000, 16);
    const auto b = pulp::audio::WaveformOverview::build_from_buffer_view(second, 8000, 16);
    const auto stitched = concatenate_levels({a.level(0), b.level(0)});
    REQUIRE(stitched.peaks_per_channel == overview.level(0).peaks_per_channel);
    for (std::uint32_t p = 0; p < stitched.peaks_per_channel; ++p) {
        CHECK(stitched.peaks[0][p].min_q7 == overview.level(0).peaks[0][p].min_q7);
        CHECK(stitched.peaks[0][p].max_q7 == overview.level(0).peaks[0][p].max_q7);
    }
    pulp::audio::OverviewLevel mismatched = b.level(0);
    mismatched.samples_per_peak = 32;
    CHECK(concatenate_levels({a.level(0), mismatched}).peaks_per_channel == 0);
}

TEST_CASE("waveform plot marks clipped columns and shades the selection",
          "[audio][image][render]") {
    WaveformColumns columns;
    columns.min = {-0.5f, -1.0f, -0.2f, 0.0f};
    columns.max = {0.5f, 1.0f, 0.2f, 0.0f};
    columns.clipped = {0, 1, 0, 0};
    WaveformImageOptions options;
    options.plot_height = 40;
    options.selection_unit = std::make_pair(0.5, 1.0);
    const RasterImage plot = render_waveform_plot(columns, options);
    REQUIRE(plot.width == 4);
    const Rgb8 marker = waveform_clip_marker_colour();
    CHECK(plot.get(1, 0) == marker);
    CHECK(plot.get(1, 39) == marker);
    CHECK_FALSE(plot.get(0, 0) == marker);
    CHECK_FALSE(plot.get(2, 39) == marker);
    // Column 3 is silent: only the zero line is drawn there, on the selection shade.
    CHECK_FALSE(plot.get(3, 5) == plot.get(0, 5));
}

// ── Goldens ────────────────────────────────────────────────────────────────

TEST_CASE("golden: 1 kHz tone spectrogram", "[audio][image][golden]") {
    const double sample_rate = 48000.0;
    SpectrogramSettings settings;
    settings.fft_length = 512;
    settings.hop = 128;
    const auto samples = sine(1000.0, 0.5, sample_rate, 24000);
    SpectrogramBuilder builder(settings, sample_rate, samples.size(), 256);
    builder.push(samples);
    SpectrogramImageOptions options;
    options.plot_height = 128;
    ImageChrome chrome;
    chrome.footer =
        "tone-1k.wav  sha256:synthetic000  fft 512 hop 128 hann linear 90dB ch0  pulp test";
    chrome.start_s = 0.0;
    chrome.end_s = 0.5;
    check_against_golden(render_spectrogram_image(builder.finish(), options, chrome),
                         "spectrogram-tone-1k-linear.png");
}

TEST_CASE("golden: linear chirp spectrogram on a log axis", "[audio][image][golden]") {
    const double sample_rate = 48000.0;
    SpectrogramSettings settings;
    settings.fft_length = 512;
    settings.hop = 128;
    settings.scale = FrequencyScale::log;
    const auto samples = chirp(100.0, 20000.0, 0.5, sample_rate, 24000);
    SpectrogramBuilder builder(settings, sample_rate, samples.size(), 256);
    builder.push(samples);
    SpectrogramImageOptions options;
    options.plot_height = 128;
    options.scale = FrequencyScale::log;
    ImageChrome chrome;
    chrome.footer = "chirp.wav  sha256:synthetic000  fft 512 hop 128 hann log 90dB ch0  pulp test";
    chrome.start_s = 0.0;
    chrome.end_s = 0.5;
    check_against_golden(render_spectrogram_image(builder.finish(), options, chrome),
                         "spectrogram-chirp-log.png");
}

TEST_CASE("golden: waveform with a clipped burst", "[audio][image][golden]") {
    auto samples = sine(440.0, 0.6, 48000.0, 24000);
    for (std::size_t i = 12000; i < 13000; ++i)
        samples[i] = (i % 2 == 0) ? 1.0f : -1.0f;
    const float* ptr = samples.data();
    pulp::audio::BufferView<const float> view(&ptr, 1, samples.size());
    const auto overview = pulp::audio::WaveformOverview::build_from_buffer_view(
        view, 48000, choose_samples_per_peak(samples.size(), 256));
    const auto columns = columns_from_overview(overview, 0, 256);
    WaveformImageOptions options;
    options.plot_height = 96;
    options.selection_unit = std::make_pair(0.25, 0.5);
    ImageChrome chrome;
    chrome.footer = "burst.wav  sha256:synthetic000  spp 64 ch0 CLIP  pulp test";
    chrome.start_s = 0.0;
    chrome.end_s = 0.5;
    check_against_golden(render_waveform_image(columns, options, chrome),
                         "waveform-clipped-burst.png");
}
