#pragma once

/// @file spectrogram_raster.hpp
/// Spectrogram image pipeline over the analysis library's STFT building
/// blocks (`window_coefficients` from audio_spectrum.hpp and the core/signal
/// FFT). Every stage is a separately callable pure function so a test or
/// another tool can verify one stage without rendering a file:
///
///   samples ──SpectrogramBuilder──▶ SpectrogramGrid (dBFS, peak-hold per column)
///          ──row_bin_bands + spectrogram_palette──▶ plot RasterImage
///          ──frequency_axis_labels + compose_plot_with_chrome──▶ final image
///
/// Display-only contract: the grid and the image never feed a verdict. The
/// dB values are honest (full-scale sine reads 0 dBFS at its bin) so a reader
/// can trust the colour bar, but anything that needs a number should call the
/// analyzers in audio_spectrum.hpp, not read pixels.
///
/// Memory contract: the builder streams. It keeps one FFT frame of carry plus
/// a grid of `columns x bins` floats, so a 70-minute file costs the same as a
/// 7-second one — only the input window moves.

#include "audio_spectrum.hpp"
#include "raster_image.hpp"

#include <pulp/signal/fft.hpp>

#include <complex>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::test::audio {

// ── Frequency axis ─────────────────────────────────────────────────────────

/// Vertical axis mapping.
///
/// `mel` uses the HTK convention, `mel = 2595 * log10(1 + hz / 700)`, which is
/// librosa's `htk=True` formula. It was chosen over the Slaney variant because
/// it is a single closed-form expression with an exact inverse, so the axis
/// has no piecewise seam at 1 kHz and `mel_to_hz(hz_to_mel(f)) == f` to
/// floating-point precision. This is a display axis, not a filterbank: no
/// energy is summed per mel band, each pixel row simply covers the FFT bins
/// whose centre falls in its mel interval (peak-hold).
enum class FrequencyScale { linear, log, mel };

std::string_view frequency_scale_name(FrequencyScale scale) noexcept;
std::optional<FrequencyScale> parse_frequency_scale(std::string_view text) noexcept;

/// HTK mel scale. Exact inverses of each other.
double hz_to_mel(double hz) noexcept;
double mel_to_hz(double mel) noexcept;

/// Position of `hz` along an axis spanning [f_lo, f_hi] under `scale`, as a
/// unit value (0 = f_lo, 1 = f_hi). Monotone in `hz`; values outside the
/// span extrapolate (callers clamp). For `log`, `f_lo` must be > 0.
double frequency_to_unit(double hz, double f_lo, double f_hi, FrequencyScale scale) noexcept;

/// Inverse of `frequency_to_unit`.
double unit_to_frequency(double unit, double f_lo, double f_hi, FrequencyScale scale) noexcept;

/// Lowest frequency the axis shows. 0 Hz for linear and mel (DC bin
/// included); the first non-DC bin, `sample_rate / fft_length`, for log,
/// because log(0) has no pixel.
double axis_low_frequency(double sample_rate, int fft_length, FrequencyScale scale) noexcept;

/// Inclusive FFT-bin range one pixel row covers.
struct BinBand {
    std::uint32_t first = 0;
    std::uint32_t last = 0;

    friend bool operator==(const BinBand&, const BinBand&) = default;
};

/// Bin band for every plot row, row 0 at the top (highest frequency). Each
/// row covers the bins whose centre frequency lies in the row's frequency
/// interval; a row narrower than one bin maps to the nearest bin so no row is
/// ever empty, and every bin from the axis floor to Nyquist belongs to at
/// least one row.
std::vector<BinBand> row_bin_bands(std::uint32_t bins, double sample_rate, int fft_length,
                                   std::uint32_t rows, FrequencyScale scale);

/// Left-axis labels at "nice" frequencies for the scale (1-2-5 decades for
/// log and mel, a linear step otherwise), as unit positions.
std::vector<AxisLabel> frequency_axis_labels(double sample_rate, int fft_length,
                                             FrequencyScale scale, std::uint32_t rows);

// ── Palette ────────────────────────────────────────────────────────────────

/// Fixed five-stop gradient (near-black navy → blue → magenta → orange →
/// pale yellow), linearly interpolated. `t` is 0 at the dB floor and 1 at
/// the ceiling; out-of-range values clamp. Luminance rises monotonically so a
/// grey-scale copy stays readable.
Rgb8 spectrogram_palette(double t) noexcept;

/// Maps a dBFS value onto the palette parameter for a display window of
/// [ceiling_db - range_db, ceiling_db].
double db_to_palette_unit(double db, double ceiling_db, double range_db) noexcept;

// ── STFT grid ──────────────────────────────────────────────────────────────

struct SpectrogramSettings {
    int fft_length = 2048; ///< power of two, at least 16
    int hop = 512;         ///< at least 1
    Window window = Window::hann;
    FrequencyScale scale = FrequencyScale::linear;
    double db_range = 90.0; ///< displayed dynamic range below 0 dBFS
};

/// Checks the settings a caller is about to render with. Returns an empty
/// string when valid, otherwise a one-line reason suitable for a CLI error.
std::string validate_spectrogram_settings(const SpectrogramSettings& settings);

/// Peak-hold magnitude grid at image column resolution. Values are dBFS:
/// a full-scale sine reads 0 dB at its bin through any window, because the
/// window coefficients are coherent-gain normalized (see
/// `window_coefficients`). Silence reads `kSpectrogramFloorDb`.
struct SpectrogramGrid {
    std::uint32_t columns = 0;
    std::uint32_t bins = 0; ///< fft_length / 2 + 1
    double sample_rate = 0.0;
    int fft_length = 0;
    int hop = 0;
    std::uint64_t frames_total = 0;    ///< STFT frames the range yields
    std::uint64_t frames_analyzed = 0; ///< STFT frames actually folded in
    std::vector<float> magnitude_db;   ///< columns * bins, column-major

    float at(std::uint32_t column, std::uint32_t bin) const noexcept {
        return magnitude_db[static_cast<std::size_t>(column) * bins + bin];
    }
};

inline constexpr float kSpectrogramFloorDb = -200.0f;

/// Largest FFT the image pipeline accepts. Beyond this a single grid column
/// is already tens of thousands of bins, far past what any plot row can show.
inline constexpr int kSpectrogramMaxFftLength = 65536;

/// Ceiling on the grid a builder may allocate: `columns * bins * sizeof(float)`.
/// 256 MiB keeps a default-width image under a few MB and refuses the
/// combinations (wide image, huge FFT) that would otherwise allocate gigabytes
/// before the first frame is read.
inline constexpr std::uint64_t kSpectrogramGridBudgetBytes = 256ull * 1024 * 1024;

/// Bytes the grid for `columns` x (fft_length / 2 + 1) bins occupies.
std::uint64_t spectrogram_grid_bytes(std::uint32_t columns, int fft_length) noexcept;

/// Number of STFT frames a range of `total_frames` samples yields with the
/// given FFT length and hop: `(total - fft) / hop + 1`, or 0 when the range
/// is shorter than one FFT. A trailing partial frame is dropped rather than
/// zero-padded, so the image never shows a truncation edge as content.
std::uint64_t spectrogram_frame_count(std::uint64_t total_frames, int fft_length, int hop) noexcept;

/// dBFS of one FFT bin magnitude for an N-point real FFT of a window with
/// coherent gain 1: `20 log10(2 |X| / N)`, floored at `kSpectrogramFloorDb`.
float bin_magnitude_dbfs(double magnitude, int fft_length) noexcept;

/// Streams samples into a `SpectrogramGrid` of fixed column count. Push
/// contiguous blocks in order (any block size); frames straddling a block
/// boundary are carried over. Column `c` holds the peak over STFT frames
/// `[c * F / columns, (c + 1) * F / columns)` where `F` is the frame count;
/// when fewer frames than columns exist, uncovered columns repeat the nearest
/// analysed column so the image has no gaps.
///
/// Pushing the whole range in one block or in many produces an identical
/// grid; that is a tested property, because the CLI feeds memory-mapped
/// windows and the unit tests feed one buffer.
class SpectrogramBuilder {
  public:
    /// `settings` must pass `validate_spectrogram_settings`. `total_frames`
    /// is the length of the range that will be pushed; samples beyond it are
    /// ignored.
    SpectrogramBuilder(const SpectrogramSettings& settings, double sample_rate,
                       std::uint64_t total_frames, std::uint32_t columns);

    std::uint64_t expected_frames() const noexcept {
        return frames_total_;
    }

    void push(std::span<const float> samples);

    /// Finalizes and returns the grid. Further pushes are ignored.
    SpectrogramGrid finish();

  private:
    void analyze_frame(const float* frame);

    SpectrogramSettings settings_;
    double sample_rate_ = 0.0;
    std::uint64_t frames_total_ = 0;
    std::uint64_t frames_done_ = 0;
    std::uint32_t columns_ = 0;
    std::uint32_t bins_ = 0;
    std::vector<double> window_;
    std::vector<double> segment_;
    // One FFT plan and one spectrum buffer for the builder's lifetime: the
    // twiddle tables are built once, not once per STFT frame.
    pulp::signal::Fft64 fft_;
    std::vector<std::complex<double>> spectrum_;
    std::vector<float> carry_;
    std::vector<std::uint8_t> column_touched_;
    SpectrogramGrid grid_;
    bool finished_ = false;
};

// ── Rendering ──────────────────────────────────────────────────────────────

struct SpectrogramImageOptions {
    std::uint32_t plot_height = 256;
    FrequencyScale scale = FrequencyScale::linear;
    double db_range = 90.0;
    double db_ceiling = 0.0;
};

/// Plot pixels only: width = grid columns, height = `plot_height`. Each
/// pixel is the palette colour of the peak dB over its row's bin band.
RasterImage render_spectrogram_plot(const SpectrogramGrid& grid,
                                    const SpectrogramImageOptions& options);

/// Plot plus frequency labels, time ticks and footer.
RasterImage render_spectrogram_image(const SpectrogramGrid& grid,
                                     const SpectrogramImageOptions& options,
                                     const ImageChrome& chrome);

} // namespace pulp::test::audio
