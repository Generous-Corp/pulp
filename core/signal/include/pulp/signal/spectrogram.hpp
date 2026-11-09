#pragma once

/// @file spectrogram.hpp
/// Color ramp mapping and frequency axis utilities for spectrogram display.

#include <vector>
#include <cmath>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace pulp::signal {

/// RGBA color for spectrogram pixels.
struct SpectrogramColor {
    uint8_t r = 0, g = 0, b = 0, a = 255;

    friend bool operator==(const SpectrogramColor&, const SpectrogramColor&) = default;
};

/// Predefined color ramp types for spectrogram rendering.
enum class ColorRamp { inferno, viridis, grayscale, heat };

/// Maps a normalized value [0,1] to a color using a predefined ramp.
///
/// RT contract: map(), set_ramp(), and ramp() are allocation-free fixed-state
/// operations.
class ColorMapper {
public:
    explicit ColorMapper(ColorRamp ramp = ColorRamp::inferno) : ramp_(ramp) {}

    /// Map a normalized value (0 = minimum, 1 = maximum) to a color.
    SpectrogramColor map(float t) const {
        // std::clamp(NaN, …) returns NaN, which the ramps then cast to
        // uint8_t. Sanitize NaN to 0 first; ±Inf clamps safely to 1.0 / 0.0
        // below.
        if (std::isnan(t)) t = 0.0f;
        t = std::clamp(t, 0.0f, 1.0f);

        switch (ramp_) {
            case ColorRamp::inferno:   return map_inferno(t);
            case ColorRamp::viridis:   return map_viridis(t);
            case ColorRamp::grayscale: return map_grayscale(t);
            case ColorRamp::heat:      return map_heat(t);
        }
        return map_grayscale(t);
    }

    void set_ramp(ColorRamp ramp) { ramp_ = ramp; }
    ColorRamp ramp() const { return ramp_; }

private:
    ColorRamp ramp_;

    struct CP { float t; uint8_t r, g, b; };

    static SpectrogramColor interpolate(const CP* pts, int count, float t) {
        if (t <= pts[0].t) return {pts[0].r, pts[0].g, pts[0].b, 255};
        if (t >= pts[count - 1].t) return {pts[count-1].r, pts[count-1].g, pts[count-1].b, 255};

        for (int i = 0; i < count - 1; ++i) {
            if (t >= pts[i].t && t <= pts[i + 1].t) {
                float f = (t - pts[i].t) / (pts[i + 1].t - pts[i].t);
                return {
                    static_cast<uint8_t>(pts[i].r + f * (pts[i + 1].r - pts[i].r)),
                    static_cast<uint8_t>(pts[i].g + f * (pts[i + 1].g - pts[i].g)),
                    static_cast<uint8_t>(pts[i].b + f * (pts[i + 1].b - pts[i].b)),
                    255
                };
            }
        }
        return {0, 0, 0, 255};
    }

    static SpectrogramColor map_inferno(float t) {
        static constexpr CP pts[] = {
            {0.0f,   0,   0,   4},
            {0.25f, 87,  16, 110},
            {0.5f, 188,  55,  84},
            {0.75f, 249, 142,   9},
            {1.0f, 252, 255, 164},
        };
        return interpolate(pts, 5, t);
    }

    static SpectrogramColor map_viridis(float t) {
        static constexpr CP pts[] = {
            {0.0f,   68,   1,  84},
            {0.25f,  59,  82, 139},
            {0.5f,   33, 145, 140},
            {0.75f, 94,  201,  98},
            {1.0f,  253, 231,  37},
        };
        return interpolate(pts, 5, t);
    }

    static SpectrogramColor map_grayscale(float t) {
        auto v = static_cast<uint8_t>(t * 255);
        return {v, v, v, 255};
    }

    static SpectrogramColor map_heat(float t) {
        static constexpr CP pts[] = {
            {0.0f,    0,   0,   0},
            {0.33f, 180,   0,   0},
            {0.66f, 255, 200,   0},
            {1.0f,  255, 255, 255},
        };
        return interpolate(pts, 4, t);
    }
};

/// Frequency axis scaling modes.
enum class FrequencyScale { linear, logarithmic, mel };

/// Utilities for mapping FFT bins to display coordinates.
///
/// RT contract: configure() and all mapping helpers are allocation-free.
class FrequencyAxis {
public:
    /// Configure the axis for a given FFT size and sample rate.
    void configure(int fft_size, float sample_rate, FrequencyScale scale = FrequencyScale::logarithmic) {
        fft_size_ = fft_size;
        sample_rate_ = sample_rate;
        scale_ = scale;
        num_bins_ = fft_size / 2 + 1;
        nyquist_ = sample_rate / 2.0f;
        low_hz_ = scale == FrequencyScale::logarithmic ? 1.0 : 0.0;
    }

    /// Lowest frequency the double-precision mapping places at position 0.
    /// `configure()` resets it to the float mapping's implicit floor (1 Hz for
    /// logarithmic, 0 Hz otherwise); an offline renderer may raise it, for
    /// example to the first non-DC bin so a log axis has no empty decades
    /// below the data. Must be below Nyquist.
    void set_low_frequency(double low_hz) {
        low_hz_ = low_hz;
    }
    double low_frequency() const {
        return low_hz_;
    }

    /// Map Hz to a normalized display position in double precision, honoring
    /// `low_frequency()`: position 0 is the floor, 1 is Nyquist. Values outside
    /// [floor, Nyquist] extrapolate so a caller can clamp or cull as it sees fit.
    double hz_to_display(double hz) const {
        const double lo = transform(low_hz_);
        const double hi = transform(static_cast<double>(nyquist_));
        if (!(hi > lo))
            return 0.0;
        return (transform(hz) - lo) / (hi - lo);
    }

    /// Inverse of the double-precision `hz_to_display`.
    double display_to_hz(double pos) const {
        const double lo = transform(low_hz_);
        const double hi = transform(static_cast<double>(nyquist_));
        return inverse(lo + pos * (hi - lo));
    }

    /// HTK mel scale, `2595 log10(1 + hz / 700)`, exact inverses of each other.
    static double hz_to_mel(double hz) {
        return 2595.0 * std::log10(1.0 + hz / 700.0);
    }
    static double mel_to_hz(double mel) {
        return 700.0 * (std::pow(10.0, mel / 2595.0) - 1.0);
    }

    /// Map a frequency bin index to a normalized display position [0, 1].
    float bin_to_display(int bin) const {
        float freq = bin_to_hz(bin);
        return hz_to_display(freq);
    }

    /// Map a normalized display position [0, 1] to the nearest bin index.
    int display_to_bin(float display_pos) const {
        float freq = display_to_hz(display_pos);
        return hz_to_bin(freq);
    }

    /// Convert bin index to frequency in Hz.
    float bin_to_hz(int bin) const {
        return static_cast<float>(bin) * sample_rate_ / fft_size_;
    }

    /// Convert frequency in Hz to nearest bin index.
    int hz_to_bin(float hz) const {
        int bin = static_cast<int>(std::round(hz * fft_size_ / sample_rate_));
        return std::clamp(bin, 0, num_bins_ - 1);
    }

    /// Map Hz to normalized display position [0, 1].
    float hz_to_display(float hz) const {
        hz = std::clamp(hz, 1.0f, nyquist_);
        switch (scale_) {
            case FrequencyScale::linear:
                return hz / nyquist_;
            case FrequencyScale::logarithmic:
                return std::log2(hz) / std::log2(nyquist_);
            case FrequencyScale::mel:
                return hz_to_mel(hz) / hz_to_mel(nyquist_);
        }
        return hz / nyquist_;
    }

    /// Map normalized display position [0, 1] back to Hz.
    float display_to_hz(float pos) const {
        pos = std::clamp(pos, 0.0f, 1.0f);
        switch (scale_) {
            case FrequencyScale::linear:
                return pos * nyquist_;
            case FrequencyScale::logarithmic:
                return std::pow(2.0f, pos * std::log2(nyquist_));
            case FrequencyScale::mel:
                return mel_to_hz(pos * hz_to_mel(nyquist_));
        }
        return pos * nyquist_;
    }

    int num_bins() const { return num_bins_; }
    float nyquist() const { return nyquist_; }
    FrequencyScale scale() const { return scale_; }

private:
    int fft_size_ = 1024;
    float sample_rate_ = 44100.0f;
    FrequencyScale scale_ = FrequencyScale::logarithmic;
    int num_bins_ = 513;
    float nyquist_ = 22050.0f;
    double low_hz_ = 1.0;

    static float hz_to_mel(float hz) {
        return 2595.0f * std::log10(1.0f + hz / 700.0f);
    }

    static float mel_to_hz(float mel) {
        return 700.0f * (std::pow(10.0f, mel / 2595.0f) - 1.0f);
    }

    // Axis transform for the double-precision mapping. log2 of a zero floor
    // is clamped to the smallest normal so a misconfigured floor degrades to
    // a very low frequency rather than -inf.
    double transform(double hz) const {
        switch (scale_) {
        case FrequencyScale::linear:
            return hz;
        case FrequencyScale::logarithmic:
            return std::log2(std::max(hz, 2.2250738585072014e-308));
        case FrequencyScale::mel:
            return hz_to_mel(std::max(hz, 0.0));
        }
        return hz;
    }

    double inverse(double value) const {
        switch (scale_) {
        case FrequencyScale::linear:
            return value;
        case FrequencyScale::logarithmic:
            return std::exp2(value);
        case FrequencyScale::mel:
            return mel_to_hz(value);
        }
        return value;
    }
};

/// Scrolling spectrogram buffer that accumulates STFT frames as rows.
/// Each row is a time slice; columns are frequency bins mapped to colors.
///
/// RT contract: configure() allocates storage and is prepare/control-thread
/// work. push_column() and accessors are allocation-free after configure().
class SpectrogramBuffer {
public:
    /// Configure the buffer.
    /// @param width  Number of time columns (scroll history depth).
    /// @param height Number of frequency rows (typically num_bins or display height).
    void configure(int width, int height) {
        width_ = std::max(0, width);
        height_ = std::max(0, height);
        pixels_.resize(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_));
        write_col_ = 0;
        frames_written_ = 0;
    }

    /// Push a new STFT frame as a column of colors.
    /// magnitudes_db: dB-scaled magnitude array, one per frequency bin.
    /// The caller maps bins to rows; this function maps dB values to colors.
    template <typename SampleType = float>
    void push_column(const SampleType* magnitudes_db, int num_bins,
                     const ColorMapper& mapper,
                     float min_db = -80.0f, float max_db = 0.0f) {
        if (width_ <= 0 || height_ <= 0) return;

        float range = max_db - min_db;
        if (range <= 0) range = 1.0f;

        for (int row = 0; row < height_; ++row) {
            // Map row to bin (simple linear mapping; for log/mel, caller
            // should pre-resample the magnitudes)
            int bin = (num_bins > 0 && height_ > 0)
                ? std::clamp(row * num_bins / height_, 0, num_bins - 1)
                : 0;

            float magnitude_db = magnitudes_db != nullptr && num_bins > 0
                ? static_cast<float>(magnitudes_db[bin])
                : min_db;
            float normalized = (magnitude_db - min_db) / range;
            pixels_[row * width_ + write_col_] = mapper.map(normalized);
        }

        write_col_ = (write_col_ + 1) % width_;
        ++frames_written_;
    }

    void push_column(std::nullptr_t magnitudes_db, int num_bins,
                     const ColorMapper& mapper,
                     float min_db = -80.0f, float max_db = 0.0f) {
        push_column<float>(magnitudes_db, num_bins, mapper, min_db, max_db);
    }

    /// Access the pixel buffer for rendering.
    /// Layout: row-major, height rows x width columns.
    /// The newest column is at (write_col_ - 1), scrolling left.
    const SpectrogramColor* pixels() const { return pixels_.data(); }

    /// Get the column index where the next frame will be written.
    int write_column() const { return write_col_; }

    int width() const { return width_; }
    int height() const { return height_; }
    int frames_written() const { return frames_written_; }

private:
    int width_ = 0;
    int height_ = 0;
    int write_col_ = 0;
    int frames_written_ = 0;
    std::vector<SpectrogramColor> pixels_;
};

} // namespace pulp::signal
